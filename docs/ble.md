# BLE 连接与设备身份

[文档导航](README.md) · 实现：[main.c](../src/main.c)、[prj.conf](../prj.conf)

本板使用 Zephyr 单连接 BLE peripheral，OpenDisplay 的发现依赖名称前缀、服务 UUID 和厂商数据。排查时应分别观察广播、连接、ATT MTU 和应用响应；某一层正常不代表下一层已经工作。

## 地址、名称与序列号

BLE 地址标识连接对象，显示名称便于人识别，两者独立。固件在有效的 FICR 工厂静态地址存在时，于 `bt_enable()` 前调用 `bt_id_create()` 建立身份。这保留了关闭厂商 HCI 前的身份来源；改名称不会改地址。macOS 将地址映射为 peripheral UUID。

名称选择规则：

1. DataExtended 的 `serial_number` 非空时，名称为 `OD` 加序列号，保留大小写。
2. 否则取 `NRF_FICR->DEVICEID[1]` 的低 24 位，格式为 `OD` 加六位大写十六进制，左侧补零。

芯片派生名称不是 BLE MAC 后三字节。GAP Device Name 与扫描响应的 Complete Local Name 使用同一内容；配置提交后更新，断开重新扫描即可看到。清空序列号或恢复默认会回退芯片名称。

传统扫描响应共有 31 B，扣除 AD 头和 `OD` 后，序列号最多显示 27 B。配置能存 31 B，较长值按 UTF-8 字符边界截断显示，原始存储不变。为避免显示重名，建议板号控制在 27 B 内。工具接受可打印、非纯空白的 UTF-8 文本，拒绝 NUL 和控制字符。

## 序列号工具

```sh
uv run --locked python scripts/set_serial.py
uv run --locked python scripts/set_serial.py scan
uv run --locked python scripts/set_serial.py scan --unset
uv run --locked python scripts/set_serial.py 'DEVICE_ADDRESS' 2402859c
uv run --locked python scripts/set_serial.py 'DEVICE_ADDRESS' --clear
```

交互模式按编号选设备，`r` 重扫、`q` 退出。默认扫描 12 秒、逐台读取配置最多 20 秒，可用 `--seconds`、`--timeout` 调整。`scan` 只读；`--unset` 保留未设置和状态未知的设备。

扫描列表按本次广告及配置读取结果生成：成功读到空序列号才算“未设置”，超时或需要密钥算“未知”。同名设备按地址区分；没有广播名称但具有服务/厂商标识的设备仍可列出。系统缓存名称会标明来源，未知状态不会在交互菜单中被直接写入。

写入只修改 serial 字段，原配置以 0600 权限保存到 `build/serial-backups/`，或 `--backup PATH` 指定的未存在文件；随后重连并比对完整配置。认证设备使用 `--key-file PATH`，内容为原始 16 B 密钥。

## MTU 为什么不等于空中包长

| 层次 | 本项目设置与作用 |
| --- | --- |
| 应用命令 | 整个 GATT 写入值最多 244 B；direct 数据另有 230 B 上限 |
| ATT | 本地支持 MTU 247，普通写入值上限为 MTU 减 3 |
| L2CAP / Host ACL | RX 缓冲 251 B 支持重组；TX MTU 247 |
| Link Layer | 保留 27 B 链路包，较大的上层包可分片 |

固定 Zephyr 版本的非 EATT MTU 取收发能力较小值。只增大 RX 而让 TX MTU 停留在 23，仍无法得到 247。反过来，保留 27 B ACL TX/链路包不代表 ATT 只能为 23；大 L2CAP 数据可以分片。

本项目启用了 nRF51 明文 DLE 能力，但最大链路包仍为 27 B。在已有 Mac 测试中，这让对端协商的 ATT MTU 从 185 提升到 247；单纯由固件主动交换 MTU 没有提升。直接把链路包也放大到 251 的候选曾超出 RAM，未采用。

这是已测设备组合的经验，不能强制所有中央设备选择 247。官方 SDK 默认 direct DATA 的 230 B 数据加命令头后为 232 B；在 MTU 185、写入值最多 182 B 的连接上无法容纳。应先记录实际 MTU 和 WWR 上限，再判断分包是否正确。

明文 DLE 与 OpenDisplay 应用层加密属于不同层。当前关闭 SMP/控制器链路加密，仍保留 CMAC/CCM。加密后的可用 payload 和握手 MTU 要求见[认证](security.md)。

## 区分大包、突发流量和栈错误

PIPE 协商 W=N=1；标准客户端按确认推进。连续突发 WWR 可能塞满深度为 1 的应用队列并触发断连，这与单个包超 MTU 是不同问题。

nRF51 的 Host 接收/GATT 回调可运行在系统工作队列，控制器还有独立普通与优先接收线程。只改 `BT_RX_STACK_SIZE` 不能推断所有接收栈都增大；检查生成配置、ELF 栈符号、故障 PC 和当前线程，确定实际溢出的上下文。

建议按以下顺序定位：

1. 扫描是否收到服务/厂商数据，名称是否来自当次扫描响应。
2. 连接后是否订阅成功，实际 ATT MTU/WWR 上限是多少。
3. 单个合法大小的包能否得到应用响应；默认 raw/zlib 上传是否能收到 `00 73`。
4. 只有连续突发才失败时，检查客户端是否遵守 PIPE 窗口与 ACK。
5. 出现复位时，用匹配 ELF 检查 fault、线程与栈，避免先盲目增加全部缓冲。

## 扫描和缓存的已知边界

已有序列号测试确认名称更新、清空回退和复位保持；同时出现过扫描只收到无名称广告甚至零设备的情况。现场读取曾确认慢广播已启用、GAP 名称及控制器 SCAN_RSP 正确，仍不足以区分空口、扫描请求响应链路与 macOS 栈的问题。因此工具保留“未知”和本次扫描快照语义，不从旧列表推断设备在线。

在同一 BLE 身份下轮换 Zephyr/Mynewt 固件，曾出现 macOS 缓存旧 GATT 句柄导致订阅失败。跨固件对照应区分缓存问题与设备端属性权限；当时采用构建时测试身份完成对照，并未修改生产身份策略。

底层依据为固定 Zephyr 的 [ATT 定义](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/att_internal.h)、[连接与分片](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/conn.c)。修改配置时应同时检查对应版本的实现与生成 `.config`。
