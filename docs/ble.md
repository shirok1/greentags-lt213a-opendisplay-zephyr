# BLE 连接与设备身份

[文档导航](README.md) · 实现：[main.c](../src/main.c)、[prj.conf](../prj.conf)

本板使用 Zephyr 单连接 BLE peripheral，OpenDisplay 的发现依赖名称前缀、服务 UUID 和厂商数据。排查时应分别观察广播、连接、ATT MTU 和应用响应；某一层正常不代表下一层已经工作。

Zephyr 4.4.2 使用 `BT_LE_ADV_OPT_CONN`，断开后由主循环重新启动快广播；连接引用尚未释放时沿用五秒重试。
ACL 接收配置为 `BT_BUF_ACL_RX_COUNT_EXTRA=1`（两个 ACL 槽），与两个事件槽使用原生共享接收池。事件数量大于 ACL TX 数量。
使用上游原生缓冲实现，ACL TX 数量为 1、Event RX 为 2，保留 MTU 247 和两个 L2CAP TX 缓冲；不再修改 Zephyr。约束见[性能文档](performance.md)。已测加密上传、配置事务及重连，任意客户端压力和真实空口丢包仍需单独验证。

当前使用 `BT_ATT_TX_COUNT=3`，应用等待每条通知的完成回调再发送下一条，给 GATT
响应及无匹配时的错误替代响应留出缓冲。回调在上游真正归还 ATT 缓冲后执行。
通知超时后仍保留占槽状态；同连接不能继续排队，主循环请求断开；跨连接递增 ticket
过滤迟到回调。`generation` 和连接引用检查保持不变。

三槽预算覆盖应用通知、GATT响应和无匹配时的错误替代响应。Zephyr的响应缓冲可能
仍在等待销毁，不能假设一次请求只需要一个立即可重用的ATT槽。通知背压避免应用
连续占用剩余槽，但不能证明任意客户端压力下永不耗尽。
`bt_gatt_notify_cb()` 内部分配仍可能等待；应用层2秒期限只约束外层重试/完成等待，
不覆盖该调用内部。超时及恢复的验证边界见 [验证指南](validation.md)。

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
| Link Layer | 初始 27 B，DLE 上限 TX 56 / RX 37 B；较大的上层包可分片 |

固定 Zephyr 版本的非 EATT MTU 取收发能力较小值。只增大 RX 而让 TX MTU 停留在 23，仍无法得到 247。反过来，较小的 ACL TX/链路包不代表 ATT 只能为 23；大 L2CAP 数据可以分片。

本项目启用nRF51明文DLE，上限TX56 B / RX37 B，实际长度由对端协商。RX37仍容纳于
现有广播/数据共享节点预算；TX56可容纳49 B加密配置通知加ATT/L2CAP头，减少分片。
ATT MTU协商与DLE相互独立，应分别记录实际值；不能从MTU247推断无线包也是247 B。

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

底层依据为固定 Zephyr 的 [ATT 定义](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/subsys/bluetooth/host/att_internal.h)、[连接与分片](https://github.com/zephyrproject-rtos/zephyr/blob/v4.4.2/subsys/bluetooth/host/conn.c)。修改配置时应同时检查对应版本的实现与生成 `.config`。

内存成本与参考吞吐见 [性能](performance.md)。
