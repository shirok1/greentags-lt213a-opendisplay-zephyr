# 系统裁剪与全局性能（2026-09-12）

两边默认已切换到全局 `-O2`，Zephyr 同时保留 LTO。此结论取代前一轮“仅 EPD 用 O2 / Mynewt 保持 Os”的配置选择。工具链为本机 Arm GNU GCC 15.3.1 20260627；没有以降低任务栈、数据池或安全检查换取链接成功。

## 裁剪内容

Zephyr 的 `prj.conf` 禁用厂商 HCI、详细蓝牙断言字符串、GATT 多属性读取、远程写入设备名、BLE Minimum Used Channels procedure、legacy 广播切换备份和内核时间片功能。应用保留本地动态命名；只使用普通非定向 legacy 广播，快慢切换不需要 directed/extended 数据备份。原时间片大小为 0，应用/蓝牙栈无修改时间片的调用，因此裁掉编译支持不改变当前调度。

厂商 HCI **不是完全未使用**：初版关闭后会丢失 Nordic 固定蓝牙身份，设备改用每次启动随机生成的地址。最终在 `bt_enable()` 前，用 `bt_id_create()` 设置 FICR 工厂静态地址；有效性判断、字节序和静态地址位与原 Nordic controller 实现一致。未启用 BLE privacy，因此不需要导入 controller 的 IR。原有 macOS 设备 UUID 已通过三次 SWD 复位后重连确认。

`att_timeout()` 在日志关闭后仍调用地址格式化，拉入 `snprintk` 等函数。`scripts/patch_zephyr.py` 只把日志相关格式化置于 `CONFIG_LOG` 条件内，保留原来的 ATT 超时断开。标准构建 `scripts/build.py` 自动应用该补丁；验证覆盖 pinned 源码匹配、重复执行不变、源码漂移报错和原断开调用保留。最终 ELF 不再有 `snprintk` 函数。直接运行裸 `west build` 前也需要先运行该补丁脚本。

Mynewt 的 `BLE_GATT_INDICATE=0`、`BLE_GATT_MAX_PROCS=0` 去掉应用不使用的 indication/客户端过程池，回收全局 O2 多出的 64 B 静态 RAM。OpenDisplay 使用 notification，GATT 表启动后不变，也没有绑定设备缓存。原生 Service Changed 特征仍由 NimBLE 注册，但此配置不再发送 indication；未来动态修改 GATT、增加绑定缓存或客户端功能时必须重新配置。编译器使用 `lt213a_o2` profile，构建脚本自动追加 profile，保留 upstream 原始优化档。

仍保留应用层 AES/CMAC/CCM、随机数、看门狗、fatal 重启、遥测、100 帧局刷、深睡、BLE MTU 247 和原数据池。没有启用 fast-math，也没有削减 BUSY 等待或 SPI 最小半周期。全局 O3 在前一轮超出资源约束，不作为默认。

## 最终资源

| 固件 | 编译参数 | Flash | RAM 布局 |
|---|---|---:|---|
| Zephyr | 全局 O2 + LTO | 120,596 B | 静态预留 16,360 B，外部余量 24 B |
| Mynewt 默认身份 | 全局 O2 | 110,460 B | 静态 13,936 B + IRQ 栈 384 B + heap 2,064 B |
| Mynewt 实板测试身份 | 全局 O2 | 110,508 B | 与默认身份相同 |

Zephyr 的静态预留包含线程栈；24 B 不是线程栈剩余空间。Mynewt heap 数值为运行前保留区，GATT 启动分配会消耗其中一部分，不能直接与 Zephyr 的余量比较。

裁剪前 Zephyr 全局 O2 需要 135,008 B，最终减少 14,412 B。相对上一轮混合优化默认（99,228 B），全局高性能仍需多 21,368 B Flash；Mynewt 相对 Os 默认增加 17,364 B。两边都有空间：Zephyr 自身 126 KiB 分区余 8,428 B；为了轮换两种固件保留 Mynewt 最后 10 KiB 配置区时，当前 Zephyr 还余 236 B。

最终 Zephyr 190 个 C 编译单元均为 O2；Mynewt 366 个对象命令均为 O2（Newt 重复同一 flag）。Zephyr power-off 对照构建为 120,556 B Flash / 16,360 B RAM。

## 复现与产物

- Zephyr：`uv run --locked python scripts/build.py`；`uv run --locked python scripts/test.py`。
- Mynewt：配置 PATH 中的 `newt` 后运行 `tools/build.sh`、`tools/test.sh`。
- 固定身份回归：`python tests/test_identity_hardware.py DEVICE`，需连接 SWD，设备必须是旧固件已知的地址/UUID。
- 本机日志和备份：`build/system-trim-20260912/`；备份不应公开。
- 原始时间轨迹：`build/profiling-20260912/mynewt-global-o2-trim-bench.json`、`zephyr-global-o2-trim-final-bench.json`。此前两个 Zephyr 扫描失败候选记录了未保留固定身份的问题，不用于性能数据。
- Zephyr BIN SHA-256：`0397e67cf347d3f2d4a789e8352b62add559fb90a6721068cbb1c7f681e5e068`。
- Mynewt 默认 BIN SHA-256：`44cc651379eb30db09167739eeb2de71daadfeaeb9aa0557e5a9cab599b28740`。

## 同板时间测量

py-opendisplay 7.14.1、MTU 247、PIPE window/ACK 2；固定 2,756 B 图，每类 3 次，另一次全刷建立局刷基线。单位秒，中位数；计时中未连接调试器。

| 固件 | 原始全刷 | 压缩全刷 | 局刷 | 原始 START ACK | 原始到 END ACK |
|---|---:|---:|---:|---:|---:|
| Zephyr 上轮仅 EPD O2 | 4.003 | 4.015 | 2.308 | 0.135 | 0.569 |
| Zephyr 全局 O2 裁剪 | 4.002 | 3.688 | 2.310 | 0.148 | 0.568 |
| Mynewt 上轮 Os | 4.514 | 4.632 | 2.499 | 0.463 | 1.065 |
| Mynewt 全局 O2 裁剪 | 4.345 | 4.394 | 2.426 | 0.344 | 0.873 |

两边本轮各 10 次更新全部收到完成响应。Mynewt 原始 START ACK 改善约 26%，到 END ACK 改善约 18%；Zephyr 的 EPD 原已使用 O2，本轮原始 START/END 基本持平，压缩到 END ACK 有小幅改善。总刷新仍受面板波形和约 0.3 s 的阶段波动影响，不能把全局 O2 理解成所有工作负载同比例加速。没有测量纯 CPU cycles、整板电流、全温区或低电压表现，不能据此推算省电百分比。

系统裁剪最初将 Zephyr 静态预留降至 16,296 B（外部余量 88 B）。完整加密回归测得 1,536 B 主线程栈未使用前缀仅 92 B，因此最终把其中 64 B 分配给主线程，栈改为 1,600 B；其他栈不减。上表性能轨迹采自增加栈空间前的同代码全局 O2 候选。

## 验证边界

两边标准构建、内存布局检查与主机测试通过；Zephyr 另构建并检查 power-off 对照配置。Mynewt 实板测试使用仅更改测试身份的 O2 候选，避免 macOS 缓存混淆不同 RTOS 的 GATT 表；默认身份版本完成构建，测试身份不写入生产配置。

Zephyr 完整实板安全回归覆盖缺少/错误密钥、CCM 篡改、重复 nonce、加密原始/压缩全刷、两次局刷和 60 秒绝对会话寿命；恢复原配置后使用无密钥连接核对序列化配置相同。此测试会更新 Zephyr 配置事务槽的写入历史，不能再要求全 Flash 备份逐字节相同。开始安全测试之前已确认交替烧录没有改变最后 10 KiB；Mynewt 不重叠的前 8 KiB 配置区应继续保持原字节。

本轮没有重新做看门狗故障注入、全温区画质验证或 Mynewt 实板加密回归。安全/稳定性功能保留不等于已经穷尽所有工作负载；尤其主线程栈水位仍需随未来加密协议修改复测。

最终 1,600 B 主线程栈版本重新通过完整加密实板回归，原配置恢复并核对成功。未复位读取 RAM：

| 栈 | 预留 B | 未使用前缀 B |
|---|---:|---:|
| z_idle_stacks | 128 | 84 |
| prio_recv_thread_stack | 448 | 220 |
| recv_thread_stack | 768 | 516 |
| sys_work_q_stack | 1024 | 436 |
| z_main_stack | 1600 | 156 |
| z_interrupt_stacks | 1024 | 520 |

线程哨兵全部完整，ISR 未设哨兵。主线程余量为 156 B；这是本轮工作负载观测值，不是最坏嵌套证明。安全测试后 Mynewt 不重叠的 8 KiB 配置区仍逐字节不变。

最终设备保留 Zephyr 全局 O2 固件，再次无密钥连接读取版本、明文压缩全刷与 0044 遥测通过，屏幕恢复 OPT / 1。
