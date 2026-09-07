# 认证／加密互通核查

2026-09-08。基准：本地固件源码、官方 Firmware `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`、已安装 py-opendisplay 7.14.1。本次只做离线源码审计和宿主机互操作测试，没有向设备写入密钥、配置或固件。结论：**正常密码学路径已与实际 SDK 跑通；错误处理与会话超时仍有具体差异，不能把它们归类成仅“待实测”。**

## 修复进展（同日）

固件已将未认证响应改为 `00 opcode FE`、完整性错误改为明文 `00 opcode FF`；重复或超出重放窗口的 PIPE DATA nonce 静默丢弃，保持传输。会话超时改为认证起始时间的绝对期限，增加了截止前合法流量不能续期的回归。

实机测试发现加密配置响应可触发主线程栈溢出。为避免 LTO 把认证和解密临时缓冲保留在主循环帧中，两函数与加密函数均禁止内联；主线程栈由 1280 调整到 1536 B，系统工作队列由 1280 调整到 1024 B，总 RAM 不变。以下审计条目保留修复前的证据。

### 修复后的实机回归

`uv run --locked python tests/test_security_hardware.py DEVICE BEFORE AFTER` 在 ODDA67FC 上通过（py-opendisplay 7.14.1，未修改客户端分包常量）：

- 临时密钥配置写入；无 key 抛 AuthenticationRequiredError，错误 key 抛 AuthenticationFailedError。
- 坏 CCM tag 抛 IntegrityCheckError；相同 nonce 的合法包随后成功，说明坏包未消耗 nonce。
- 已开始 PIPE 传输中重复发送同 nonce DATA，随后下一序号 DATA 正常 ACK，传输和会话未中止。
- 加密 raw / zlib 全刷、先白 100 帧再目标 100 帧的局部刷写通过；最终价格 13.81。
- 认证后 50 秒的合法业务成功，61 秒请求收到认证过期错误；绕过 SDK 的主动重认证验证固件绝对期限。
- 最终通过加密会话恢复原配置，重新无 key 连接，并比较序列化配置一致；临时 key 文件已删除。

日志 `build/auth-hardware.log`，原始配置 Flash 备份 `build/auth-before-config-flash.bin`。最初实机测试还复现了加密回复栈溢出导致重启，修复后完整测试无此失败。配置 Flash 的真实掉电原子性、换 key、低 MTU peer 仍不是这次测试覆盖项。

## 已实际验证

运行 `uv run --locked python tests/test_security.py`：通过 CMAC KDF、相互证明、CCM、篡改／重放拒绝、过期和速率限制；这是现有独立 OpenSSL 原语测试。

新增离线实验 `build/audit_sdk_auth.py`（忽略目录内实验文件）将实际 `OpenDisplayDevice.authenticate()` 的假传输接到 `tests/security_bridge.c` 导出的 C 实现，而非重新实现 Python 握手，结果：

- 官方 SDK 完整两步握手、动态 device_id 和 server proof 验证通过。
- SDK 加密 → C 解密：0、154、202、213 字节 payload 均通过，对应 31、185、233、244 字节 wire。
- C 加密 → SDK `_read()`：普通 ACK、配置响应、PIPE `highest_seen=FE` 均通过。
- 当前错误 `FE 40` 被 SDK 原样返回；预期 `00 40 FE` 会抛出 `AuthenticationRequiredError`；`00 71 FF` 会抛出 `IntegrityCheckError`。这是具体兼容失败的离线复现。

算法、互认证证明和 framing 的官方依据见 [Firmware encryption.cpp](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/encryption.cpp)、[官方 SDK 仓库](https://github.com/OpenDisplay-org/py-opendisplay)。本次 SDK 行为依据本地安装版本 `opendisplay/crypto.py:90-151` 和 `device.py:743-826`，不声称链接中的最新分支等于 7.14.1。

## 确认的缺口

### 1. 未认证响应格式错误

`src/main.c:173-175` 发送 `{FE, opcode}`；官方明确发送 `{00, opcode, FE}`。SDK `_read()` 只识别长度为 3、第三字节为 FE 的认证错误。因此无密钥访问及过期后的访问不会进入官方 SDK 的正确认证错误分支。需要改实际 dispatch 响应，单改 `send_response` 的封装判断没有用。[官方未认证与短包响应](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L733-L748)

### 2. 解密失败会断连，错误与 PIPE 恢复语义不一致

`src/main.c:177-178` 将解密错误直接上抛，主循环 `:312-315` 会中止事务并断开 BLE；没有发官方三字节 `00 opcode FF`，客户端通常只能看到断连。官方对 tag／其他完整性失败回复明文三字节错误；对 PIPE DATA 的重复 nonce／窗口外 nonce 则不发回复并保持会话，由 PIPE 超时重传恢复。当前 C 将重复和窗口外均报告 `-EALREADY`，能用于这种分类，但不要把 tag 失败也静默吞掉。[官方解密失败分类](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L767-L805)

`src/main.c:148` 还以第一字节 FE 排除加密；官方以第三字节 FE/FF 判断状态，特例排除 7 字节 PIPE ACK 的滚动序号。对于新增的错误通知，使用 `send_wire()` 明文发送是最简单明确的做法；不能把所有第一字节 FF 的协议 NACK 都当认证错误。[官方 response 封装](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L378-L405)

### 3. 会话寿命语义不同

`src/security.c:36-41` 按 `activity` 算 timeout，成功解密 `:100` 会重置它，因此持续活动能无限延长同一会话。官方按认证时的 `session_start_time` 算绝对年龄，activity 是另一字段；SDK 也按认证年龄在 90% 时主动更新。这是策略差异，正常 SDK 提前重认证掩盖了它。固件可以复用现有字段记录认证起始时刻、不在解密时重写，避免增加 RAM。应增加“持续合法流量也会在截止时过期”的测试。[官方 timeout](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/encryption.cpp#L255-L272)

## 已核查、没有发现阻断正常互通的问题

- **Nonce**：本机 RX 从 0 开始、32 位回放窗口；TX 从 `2^63` 起，拒绝 RX 高半区，避免双向同 key/nonce。官方 SDK 解密直接使用传入的 nonce，不要求 TX 从 0 起，实验已通过。无需为了字节一致取消双向隔离。SDK 的接收函数自身没有响应重放窗口，此项属于客户端能力，不是本机能单方补齐。
- **MTU**：wire 固定额外 29 字节（相对 2 字节命令＋payload）；本机最大 wire 244、payload 213。官方 SDK 加密图像 chunk 为 154；配置最大首包 payload 202，wire 233。在之前实测的 ATT MTU 247 下均可容纳。实验配置分包 wire 长度为 233、231、120。若其他客户端协商更低 MTU，配置包上限仍需另外测试，不能据此声称任意 peer 都能加密传大配置。
- **响应栈缓冲**：`send_response` 的加密输出 64 字节；本机配置明文响应最多 20 字节，wire 49；现有 ACK 等更短，正常路径不溢出。将来增加大响应需重新核对容量。
- **断连／重认证**：`src/main.c:269` 清会话；握手前 `:164-166` 中止图像和配置事务；挑战单次使用、30 秒有效；速率限制跨重连保留。原子配置提交前不会切换活动配置。
- **配置切换**：`src/protocol.c:132-146` 在提交后发 ACK，主循环 `src/main.c:300-304` 再清会话并更新广播，最终 ACK 仍用旧会话加密。客户端下一次访问需重新建立会话；正式 SDK `write_config()` 不自动更新本对象缓存密钥或清会话，换 key 后应断开并用新 key 重新构造连接。尚未真实 BLE 验证。
- **rewrite_allowed**：`src/config_store.c:56` 明确拒绝 SecurityConfig 的 flags 非零值，因此不会假装支持无旧 key 覆写。官方 SDK 支持该可选 provisioning 能力；本机是主动限制，不是偷偷忽略这个旗标。[官方配置覆写门控](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp#L554-L566)

## 在启用实机密钥前应完成的验证

1. 修前三项，回归无 key、错 key、坏 tag、重复 PIPE nonce；合法流量后的超时亦需断言。
2. 用临时测试密钥、保存原配置，实际 SDK 连接后读取版本／配置、raw 与压缩 PIPE、局部写，重连再次握手。测量认证／加密路径栈水位，不能沿用未加密刷图的栈结论。
3. 验证配置写入中断保持旧配置，提交后重连用新配置；完成后恢复原无密钥配置并再次验证广播与刷图，避免把设备留在未知 key 状态。
4. 保留 SWD 可恢复，尤其在启用密钥或换 key 测试期间。本次没有执行这些硬件步骤，不把离线通过当作无线／栈稳定性通过。

### 复现实验命令与 MTU 下限补充

```sh
uv run --locked python tests/test_security.py
uv run --locked python build/audit_sdk_auth.py
```

实际输出关键行：

```text
SDK authenticate() + C handshake and mutual proof: PASS
SDK encrypt -> C decrypt: payload 213, wire 244 PASS
C encrypted ACK/config/PIPE seq FE -> SDK _read(): PASS
Error fe40: SDK returns fe40
Error 0040fe: SDK raises AuthenticationRequiredError
Error 0071ff: SDK raises IntegrityCheckError
Encrypted config wire lengths: [233, 231, 120]
```

TX high-bit nonce 在上述 C→SDK 测试中实际使用（第一次为 `2^63`），SDK 接受。未认证错误 `FE40` 本身**没有抛认证异常**，后续 API 校验可能抛普通协议异常，不能把这次 `_read()` 测试说成后续具体异常已测得。

挑战响应 23 字节，通知需要 ATT MTU 至少 26；第二步请求 34 字节，常规单包写需要 ATT MTU 至少 37；最小加密帧 31 字节，至少 MTU 34。因此默认 ATT MTU 23 / 20 字节 payload 不能完成这里的握手。实际 SDK 加密配置首包 233 字节，需 MTU 至少 236；本机已测 247 满足。没有实测其他 peer 的 MTU 协商，也没有实现应用层分片来支持 MTU 23。
