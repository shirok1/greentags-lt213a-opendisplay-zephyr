# 认证与加密

[文档导航](README.md) · 实现：[security.c](../src/security.c)、[main.c](../src/main.c)

认证是 OpenDisplay 的应用层协议，与 BLE 配对或链路层加密独立。默认配置不启用认证；启用后，版本查询与认证交互仍可明文进行，其他业务请求必须通过会话加密。

## 配置与握手

SecurityConfig 使用 TLV `0x27`。启用时必须提供非零 128 位密钥；本实现拒绝非零安全 flags，不提供绕过旧密钥的 rewrite/provisioning 扩展。实际配置由[容器与事务校验](protocol.md)保护。

握手分两步：客户端请求随机挑战，设备返回 16 B server challenge 与 4 B device_id；客户端提交自身 challenge 和 CMAC 证明，设备验证后派生会话密钥、会话 ID 并返回 server proof。客户端验证证明后才建立双向可信会话。

本项目认证 device_id 取 `DEVICEID[0]` 并按协议编码；BLE 名称回退取 `DEVICEID[1]` 的低 24 位，二者用途不同。密码学算法遵循项目固定的 [OpenDisplay encryption.cpp](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/encryption.cpp)，主机测试用独立 OpenSSL 原语交叉验证，维护时不要用仅双方共享同一实现的测试替代它。

挑战单次使用，包括错误证明；30 秒后失效。认证请求按一分钟窗口限速，限速状态跨断连和重认证保留。开始新握手会中止未完成图像/配置事务，避免旧会话数据进入新会话。

## 密码学实现边界

固件通过 `src/crypto_zephyr.c` 调用控制器的公开 `bt_encrypt_be()`，复用 nRF51 ECB
外设，不再编译 TinyCrypt。`src/crypto.c` 实现 AES-CMAC 和固定 OpenDisplay CCM 参数，
主机用独立 AES-block 后端编译同一模式层，并与 OpenSSL CMAC/AESCCM 交叉验证。
完整 tag 比较、失败载荷清理及重放窗口提交顺序保留；模式实现仅支持 13 B nonce、
2 B AAD、12 B tag 和不超过 214 B 的加密正文，接口容量/重叠约束见 `src/crypto.h`。

此后端替换尚无实机密码学和栈水位证据。Zephyr 的 ECB 局部参数副本未主动擦除，
应用模式层的清理不覆盖已返回的驱动栈帧；不应宣称完整密钥擦除。
内存和栈对照见[加密库与 RAM 预算](crypto-memory-options.md)。

## 帧格式与 MTU 预算

加密 wire 保留两字节命令头，后接会话 ID、计数器、加密的长度与 payload，以及 12 B CCM tag。相对原始完整命令增加 29 B；CCM 使用派生出的 13 B nonce，命令头作为附加认证数据。

| 内容 | wire 长度 | 普通单包 ATT 所需 MTU |
| --- | ---: | ---: |
| 挑战响应通知 | 23 B | 至少 26 |
| 第二步认证请求 | 34 B | 至少 37 |
| 空 payload 加密命令 | 31 B | 至少 34 |
| SDK 大配置首包（202 B payload） | 233 B | 至少 236 |
| 本机最大加密 payload 213 B | 244 B | 至少 247 |

默认 MTU 23 不能完成此握手；没有额外应用分片来补齐这个限制。已测 MTU 247 支持官方 SDK 的正常配置分包，但任意低 MTU peer 的互通仍需单独验证。`send_response()` 的加密输出暂存为 64 B，当前配置明文响应最多 20 B、加密后 49 B；增加响应长度时必须同步复核容量和栈。

## 重放与会话期限

接收端接受低半区计数器并保留 32 位重放窗口；发送端从 `2^63` 开始，隔离两个方向在同一密钥下的 nonce。成功校验 tag 后才更新接收窗口，所以损坏包不会抢占合法包的计数器。

SecurityConfig 的非零超时表示从认证成功开始计算的**绝对期限**，合法业务流量不会续期。超时检查在分发请求时执行；过期会清会话并中止相关事务。断连和已提交配置变化也会清会话。SDK 可能提前主动重认证，测试固件期限时应避免客户端自动更新掩盖结果。

## 错误响应也是互通的一部分

| 情况 | 线上行为 |
| --- | --- |
| 启用认证但没有有效会话 | 明文 `00 opcode FE` |
| CCM 完整性或其他解密错误 | 明文 `00 opcode FF` |
| PIPE DATA 重复/窗口外 nonce | 静默丢弃，保留会话和传输，由 PIPE 恢复 |
| 普通业务 NACK | 仍按业务响应路径处理，不等同于认证错误 |

不要按“字节里出现 FE/FF”一概绕过加密；PIPE ACK 序号也能等于这些值。当前认证错误直接通过 `send_wire()` 发送，普通响应通过 `send_response()` 封装。

## 修改配置和密钥

当前实现先用旧会话发送最终配置 ACK，再重置会话。客户端写完后应重新建立认证；换 key 时断开并以新 key 构造连接，不依赖旧 SDK 对象自动更新密钥缓存。

配置提交保持双槽事务语义，未完成写入不会切换密钥。清除配置回到无认证默认，但旧槽可能仍包含历史密钥，因此逻辑清除不构成密钥安全销毁。

## 验证和维护重点

运行 `uv run --locked python scripts/test.py` 会生成共享配置 fixture，再执行密码学测试。直接单跑 `tests/test_security.py` 需要这些 fixture；主机 AES 后端使用 uv 管理的 cryptography，无需下载 TinyCrypt。

已有实机回归覆盖缺少/错误 key、坏 tag 后相同 nonce 合法包、重复 PIPE nonce、加密 raw/zlib 全刷和局刷、60 秒绝对期限、恢复无 key 配置。完整换 key、低 MTU peer、真实掉电仍不是这些结果覆盖的能力。

认证、加密和解密函数的 `noinline` 有资源原因：LTO 曾把密码学临时对象留在调用者帧中，引发主线程溢出。调整函数边界或编译参数时要重测加密路径栈，不能只检查明文上传。实机脚本的前提、副作用和恢复步骤见[验证指南](validation.md)。
