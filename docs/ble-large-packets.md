# BLE 大包接收调查

调查基线：`west.yml` 固定 Zephyr `9f824289b28d7aea2eee74f62787c385a5005453`，本地 `.deps/zephyr` HEAD 相同，`VERSION` 为 3.7.1。本文同时记录源码依据与本板/macOS 实测，其他中央设备仍需分别验证。

## MTU 应在固件解决

当前 `py-opendisplay 7.14.1` 的默认值是 `CHUNK_SIZE=230`、`MAX_START_PAYLOAD=200`；直接写入的数据帧有额外的命令字节，不能把 230 当作整个 ATT 包大小。客户端普通上传使用 Write Without Response，但每块仍等待应用层 ACK，所以正常直接上传只有一块在途。[官方包 7.14.1](https://pypi.org/project/py-opendisplay/7.14.1/)

Zephyr 的非 EATT 本地 ATT MTU 是 `min(BT_L2CAP_RX_MTU, BT_L2CAP_TX_MTU)`，RX MTU 是 `BT_BUF_ACL_RX_SIZE - 4`。因此目标 ATT MTU 247 需要 `CONFIG_BT_L2CAP_TX_MTU=247`、`CONFIG_BT_BUF_ACL_RX_SIZE=251`，可承载最多 244 字节的普通 ATT 写入值。客户端和服务器交换 MTU 后采用双方较小值；不能仅放大接收缓冲而保留 TX MTU 23。[ATT 定义](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/att_internal.h#L27)、[L2CAP 定义](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/include/zephyr/bluetooth/l2cap.h#L36)、[官方 MTU 示例](https://docs.zephyrproject.org/3.7.0/samples/bluetooth/mtu_update/README.html)

## 不必扩大空中链路层包

`CONFIG_BT_BUF_ACL_TX_SIZE=27` 不会将 ATT MTU 限制为 23：Host 支持把大的 L2CAP PDU 分成多个 ACL 数据包。接收方向，`bt_acl_recv()` 把首包保存在 `conn->rx`，将后续包追加到该缓冲；剩余空间不足才丢弃不完整帧。这解释了为何必须扩大 RX 缓冲，却可以保留小的 ACL TX 缓冲。[ACL Kconfig](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/common/Kconfig#L9)、[接收重组](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/conn.c#L341)

此版本发送分片实际使用 `CONFIG_BT_CONN_FRAG_COUNT` 的零数据区 view 池，默认数量随连接数。虽然 `Kconfig.l2cap` 仍有 `BT_L2CAP_TX_FRAG_COUNT`，本地源码没有 C/H 引用，不能根据旧帮助文字认定增加该选项能修好分片。[实际分片池](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/conn.c#L114)、[实际计数默认值](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/Kconfig#L277)

nRF51 的 Zephyr 控制器有 `BT_CTLR_DATA_LENGTH_CLEAR` 明文 DLE 支持。ATT 大包在协议上不要求 DLE，但中央设备的 MTU 策略可能依赖它。本次实测启用该能力、最大链路包长仍为 27，就使 Mac 的 ATT MTU 从 185 变为 247；没有将空中包长扩到 251。[nRF51 明文 DLE 配置](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/controller/Kconfig.ll_sw_split#L313)

## RAM 与崩溃定位

该 QFAB 型号只有 16 KiB SRAM，因此需要根据实际 map 和栈使用量调节，而不是全面放大缓冲。[芯片内存定义](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/dts/arm/nordic/nrf51822_qfab.dtsi#L14)

- nRF51 默认 `BT_RECV_WORKQ_SYS=y`：普通 Host HCI 接收及 GATT 回调运行在系统工作队列，实际栈由 `SYSTEM_WORKQUEUE_STACK_SIZE` 控制。单独增加 `BT_RX_STACK_SIZE` 不会扩大该执行上下文。[Host 接收上下文](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/Kconfig#L75)
- 控制器还独立创建 `recv_thread_stack`（默认 768）和 `prio_recv_thread_stack`（默认 448）。控制器接收线程执行 `process_node()`、缓冲处理及 Host 接收入口；不能把该线程的栈溢出误判成 Host 工作队列栈不足。[独立栈配置](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/controller/Kconfig.ll_sw_split#L107)、[线程与调用路径](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/controller/hci/hci_driver.c#L674)
- ACL RX 数量最小为 2，且至少为连接数加 1。正在重组的首包会占用一个缓冲，另一个用于接收续包。RX 缓冲增大时需要一起检查静态 RAM 使用。[缓冲数量约束](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/common/Kconfig#L82)
- 本项目 `src/main.c` 命令队列深度为 1；队满会主动断连。因此连续无应用 ACK 的突发流量应与单个大包分开测试，避免把应用队列溢出当作 MTU 故障。`OD_MAX_FRAME=244` 已可容纳 ATT MTU 247 对应写入值。[项目接收队列](../src/main.c)、[项目帧大小](../src/protocol.h)

建议实机依次记录：交换后的 MTU；单个 232 字节 DATA 写入；默认参数普通/压缩完整上传及重连；故障 PC、当前线程和所有相关栈边界。栈溢出或硬 fault 的根因须以 DAP 现场证据确认，以上源码推导本身不能证明是哪一个线程出错。

## 后续现场证据：对端选择了 MTU 185

主调试任务反馈：默认 230 字节 DATA 首块超时，DAP 未发现 fatal；CoreBluetooth 的 `client.mtu_size=185`、最大 Write Without Response 长度为 182，板端 ATT channel 的 RX MTU 为 247、TX MTU 为 185。因此这次失败不能归因于服务器仍只支持 20 字节；232 字节的 DATA 帧超过了本次协商后的 182 字节写入值上限。该记录是本次设备与 Mac 组合的现场测量，不是对所有 Apple 客户端的固定限制。

另一项排查结论：当前关闭 HCI ACL flow control 时，Host 的 HCI 接收池由事件和 ACL 共享，数量取两者配置的较大值。控制器 `process_node()` 分配 ACL 缓冲使用 `K_FOREVER`，所以“两个缓冲用完就直接丢弃续包”与这条源码路径不符。[共享池](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/buf.c#L51)、[阻塞分配](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/controller/hci/hci_driver.c#L456)

Apple 当前《Accessory Design Guidelines》R30（2026-06-08）58.7、58.11 节说明，Apple 中央设备根据连接条件选择数据包长度及 ATT MTU；选择 MTU 的因素包括最大数据长度。支持 DLE 的附件应先执行数据长度更新，再发起 MTU 交换。这给“启用 nRF51 明文 DLE 后，Mac 是否选择更大 MTU”提供实验依据，但没有承诺特定设备必然从 185 变成 247。[Apple 官方指南，350–352 页](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf)

另一个成本较低的实验是固件启用 GATT client 支持并主动调用 `bt_gatt_exchange_mtu()`。该 API 每条连接只允许本端主动请求一次；它仍不能强制对端声明更大的接收能力。实测主动交换仍得到 185，该尝试已撤回。[Zephyr 主动交换实现](https://github.com/zephyrproject-rtos/zephyr/blob/9f824289b28d7aea2eee74f62787c385a5005453/subsys/bluetooth/host/gatt.c#L3746)


## 最终固件与验证

配置启用 `BT_DATA_LEN_UPDATE`、`BT_CTLR_ADVANCED_FEATURES`、`BT_CTLR_DATA_LENGTH_CLEAR`，
保留 `BT_CTLR_DATA_LENGTH_MAX=27` 与 `BT_BUF_ACL_TX_SIZE=27`。
单连接、不使用 PHY 更新的本项目将本地主动控制过程上下文设为 4，避免自动数据长度更新默认配置分配 6 个上下文。
没有减小任何线程栈，没有改第三方包，也没有放宽 ATT 越界检查。

实验结果：

| 方案 | 结果 |
| --- | --- |
| 原固件，包的默认 230 B DATA | 首块超时；客户端 MTU 185 / WWR 182，DAP 读到本地 RX MTU 247 / TX MTU 185，无 fatal |
| 固件主动 MTU 交换 | 仍为 185；已撤回 |
| DLE 开启，链路最大包长 251 | RAM 超出 1,560 B；未烧录 |
| DLE 开启，链路最大包长 27、4 个本地控制上下文 | MTU 247 / WWR 244，默认大包完整上传并返回刷新完成 |

最终同工具链构建：95,172 B Flash / 16,312 B RAM；相比原基线增加 2,168 B Flash / 144 B RAM。
剩余 72 B 是静态分配之外的空间，不代表线程栈余量，也不是动态堆（堆仍为 0）。

`tests/test_upload_hardware.py` 直接导入官方包，不导入本项目上传封装，不修改 `CHUNK_SIZE=230` 或
`MAX_START_PAYLOAD=200`。它默认反复新建连接，对同一张已按原生方向准备的图执行普通和压缩上传，
每次都必须收到刷新完成。`max_queue_size=1` 仅避开本项目现有 PIPE 显式 END 与上游自动完成流程的差异，
不改变数据块或 START 大小。这个独立的 PIPE 差异不在本次 MTU 修复范围内。

```sh
uv run --locked python tests/test_upload_hardware.py DEVICE native-image.png --cycles 2
```

明文 DLE 不支持链路层加密；本项目原来已关闭 SMP 和控制器 LE 加密。
应用层 OpenDisplay AES/CCM 与链路层加密是两回事，其功能代码未因 DLE 配置而删除。
最终协商值始终取决于双方能力；该修改不能强迫任意中央设备接受 247。


两轮硬件回归均通过（raw、compressed、raw、compressed），期间无需重置 MCU。
随后通过 DAP 读取初始化填充值：主线程栈剩余未触碰前缀 108 B，系统工作队列 636 B，
控制器接收线程 548 B，优先接收线程 196 B，idle 84 B；这些线程的栈哨兵完整。
ISR 栈有 544 B 未触碰（不设线程哨兵）。这些是该测试流程的水位，不是所有认证、配置、异常路径的最坏值。

2026-09-08 更新：PIPE 原始全帧自动完成已补齐，实机回归和上传脚本已移除上述 max_queue_size=1 绕行；参见 [兼容性核对](official-compatibility.md)。
