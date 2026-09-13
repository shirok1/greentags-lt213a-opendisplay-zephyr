# LT213A 同板实机 profiling（2026-09-12）

## 初次 profiling 结果（修复前）

Zephyr 完成了相同负载的重复上传、全刷、局刷和运行中 PC 采样。Mynewt 写入校验成功，但本轮未取得正常 BLE 连接：分别观察到启动校准忙等，以及重启后的无进展 idle，不能给它填写正常上传速度或与 Zephyr 比较 CPU 利用率。

结束后已恢复原 Zephyr HEX、写后校验并复位；Flash 最后 10 KiB 与测试前备份逐字节一致。初次 profiling 未修改两边固件源码或配置；后续修复见下节。报告中的 Mynewt 故障是本轮实板观察，不代表所有 nRF51822/Mynewt 设备。

## 同日修复与补测

Mynewt 修复两处实板复现问题：启动先停止并重新启动 LFRC，避免仅依赖复位后保留的 Running 状态；启动等待各限约 1 秒并超时复位。另修复原始 PIPE 全帧收齐后没有自动 END ACK / 刷新的 SDK 互等问题；压缩和局刷仍使用显式 END。两处回归均先观察失败，再验证修复。最终默认地址构建又通过连续 3 次复位，系统 tick、RTC0/RTC1 均推进；32 项 Python 测试、62,720 次 sanitizer 协议变异、96 次 zlib 往返及驱动/遥测测试通过。

修复后的默认 Mynewt BIN 为 93,032 B（增加 96 B），SHA-256 `83f75cc3b9f6edd57de415b1e6986ac99180610c695f7027aa04bfd784134e1a`；静态 RAM 13,936 B、IRQ 栈 384 B、初始可用 heap 2,064 B 均不变。产物与回归日志保存在 `build/mynewt-clock-fix/`。

macOS 对相同 BLE 身份保留了 Zephyr 的 GATT 句柄，切换 Mynewt 后订阅失败（Writing is not permitted），关闭 Bleak 服务缓存选项未解决。因此 BLE 补测使用仅构建时覆盖的测试地址 `02:84:F6:BD:21:3A`；默认构建已恢复原身份。测试地址 ELF 为 `test-address-fixed.elf`，应用逻辑与默认构建一致，测试 BIN 的 SHA-256 为 `e6d0ad21fb861ab1e17df9228915621f07a3b5598d48205b89ead939c5150b84`。未改动系统全局蓝牙缓存。

重复负载与先前 Zephyr 相同，各 3 次，并另做一次局刷基线全刷。全部 10 次均收到 `00 73`，MTU 247；补测脚本额外打印命令前缀，调试会话未在计时期间运行。

| 端到端中位数 | Zephyr | 修复后 Mynewt | Mynewt 相对变化 |
|---|---:|---:|---:|
| 原始全刷 | 4.872 s | 5.230 s | +7.3% |
| zlib 全刷 | 4.799 s | 5.385 s | +12.2% |
| 100 帧波形局刷 | 2.907 s | 2.996 s | +3.1% |

Mynewt 原始/压缩 START ACK 中位数约 0.807/0.808 s，Zephyr 为 0.433/0.418 s；END ACK 后全刷段 Mynewt 约 3.823/3.826 s，与 Zephyr 3.821/3.792 s 接近。本次差异主要在刷新前阶段，不能据此归因于 RTOS 本身。仍未测整板电流，不能给出能耗排名。

工作后 20 秒采样 1,331 次，1,317 次（98.95%）命中 Mynewt idle，1,318 次观测睡眠，0 次 halt；该窗口与 Zephyr 的广播阶段未严格同步，仅作为恢复正常空闲的证据。RAM 快照系统 tick 为 9,824；display/main/BLE LL/idle 栈未使用前缀分别为 808/696/744/128 B。不能把样本比例直接换算能耗。

补测结束已恢复冻结的 Zephyr，Flash 写后校验通过，最后 10 KiB 配置与本轮开始前备份一致。首次恢复后扫描失败，再复位后重新发现 OD84F6BD，读取版本 `19a656b3cc4a` 与 `00 44` 遥测成功。

原始结果：`build/profiling-20260912/mynewt-fixed2-bench.json`。以下保留初次测试过程，便于审计故障与修复前后差异。

## 固定条件（初次测试）

- 同一标签、RV CMSIS-DAP `202608131200`、SWD 1000 kHz；当前 Zephyr 名称 `OD84F6BD`。
- 两个上一轮对齐后的构建，测试前冻结到 `build/profiling-20260912/`。版本查询只包含 Git HEAD，不能标识未提交改动，因此以下二进制哈希才是本次版本依据。
- Zephyr BIN：96,260 B，SHA-256 `d3405c89d5d727e55b9437dc6934d490ce530d96f93cadef37453795cf2f847f`。
- Mynewt BIN：92,936 B，SHA-256 `bad379e3665df32aab577dc8e5c9d64a563a97097d64f25a294f155e82edeff1`。
- macOS、py-opendisplay 7.14.1、同一 Python 环境；客户端请求 PIPE window/ACK interval 2，固件按自身上限协商。Zephyr 实际 ATT MTU 247。
- 104×212、2,756 B 单色图，固定随机种子 213；每字节从 16 个灰度式位模式中选取。局刷窗口 `(32,64,32,16)`，往返更新。
- full_raw、full_zlib、partial 各 3 次；局刷前另做一次压缩全刷建立 ETag。所有成功必须收到 `00 73` 刷新完成响应。本轮没有启用应用层加密。
- 基准计时期间无 OpenOCD/probe-rs 会话运行；调试器物理连线保持连接。两边均无整板电流测量，供电方式没有在本轮独立确认。

## Zephyr 时间分解

单位秒。端到端计时包含主机 SDK 调用；刷新段从主机读到 END ACK 至 SDK 收到刷新完成，包括面板刷新、BUSY 等待和关电，并非纯面板波形时间。

| 负载 | 总时间中位数 | 最小–最大 | START ACK 中位数 | 到 END ACK 中位数 | END ACK 后中位数 | 应用帧 / 字节 |
|---|---:|---:|---:|---:|---:|---:|
| 原始全刷 | 4.872 | 4.828–4.873 | 0.433 | 1.051 | 3.821 | 13 / 2,804 |
| zlib 全刷 | 4.799 | 4.753–4.812 | 0.418 | 1.004 | 3.792 | 9 / 1,500 |
| 100 帧局刷 | 2.907 | 2.890–2.910 | 0.299 | 0.404 | 2.503 | 3 / 106–107 |

连接及自动读取配置首次为 1.488 秒；这是一次观测，不能作为连接延迟分布。应用帧字节统计不含链路层分片、ATT/L2CAP 开销或射频重传。

本图压缩减少约 46.5% 应用传输字节，总时间中位数仅减少约 1.5%。全刷约 79% 的总时间位于 END ACK 之后。这个负载中，优化 PIPE/解压/SPI 的潜在收益主要集中在约 1 秒的前段；面板等待是主要时长来源。局刷总时间约为原始全刷的 60%，但不能据此换算刷新电荷。

## 运行中采样与栈

使用 OpenOCD Tcl `read_memory` 读取 DWT_PCSR (`0xe000101c`) 和 DHCSR (`0xe000edf0`)，每轮随机等待 6–14 ms。两个寄存器不是同时读取。采样期间不 halt，DHCSR 的 halted 状态观测均为 0。当前板 PCSR `0x150b4` 正好对应 Zephyr 的 `WFI`，与反汇编一致。

| 采样窗口 | 样本数 | PC 命中 idle | DHCSR 睡眠观测 |
|---|---:|---:|---:|
| Zephyr 上传断连后约 20 秒，快广播阶段 | 1,459 | 1,449（99.31%） | 1,450（99.38%） |
| Zephyr 约 32 秒，包含连接、上传/刷新及空闲尾段 | 2,349 | 2,178（92.72%） | 2,183（92.93%） |

第二个窗口不是连续纯上传负载，不能把 7.28% 直接称为“上传 CPU 利用率”。非 idle 样本出现 GPIO/软件 SPI、uzlib `decode_symbol`、BLE ticker/mayfly 等路径，与端到端时间显示的大量等待相符。短 ISR、USB/SWD 访问开销和有限采样率会影响分布；这些是观测比例，不是精确指令周期统计。

本机 probe-rs 0.32.0 的 `profile` 命令在解析参数时报告缺少 `reset`（带/不带该开关均如此），因此没有用其输出作结论。OpenOCD 内置 `profile` 试运行到 10,000 个样本即结束，正式窗口改用显式控制时长的 Tcl 采样脚本。方法参考 [OpenOCD profiling 文档](https://openocd.org/doc/html/General-Commands.html)。

上传后暂停一次、转储 16 KiB RAM，再继续运行。按 ELF 栈符号扫描初始化填充值，Zephyr 任务栈先检查并跳过 4 B 哨兵：

| Zephyr 栈 | 分配 B | 未使用前缀 B |
|---|---:|---:|
| main | 1,536 | 580 |
| system workqueue | 1,024 | 380 |
| controller RX | 768 | 548 |
| priority RX | 448 | 196 |
| ISR | 1,024 | 496 |
| idle | 128 | 84 |

任务栈哨兵均完整。结果只覆盖本次明文负载，不能替代加密和最坏中断嵌套的水位。OpenOCD 在转储结束附近记录过 external reset；RAM 是此前转储的数据，正常时延基准不包含这次调试暂停。

## Mynewt 的阻塞证据

1. 原始冻结 HEX 写入和读回校验成功。尝试连接原 BLE 标识失败；随后独立扫描 35 秒未见任何 OpenDisplay 标签，因此不能只归因于设备名或 macOS UUID 改变。
2. 初始状态采样约 20 秒：1,339 个 PC 样本全部为 `0x13e84/0x13e86`，对应 [`calibration_init()`](../../greentags-lt213a-opendisplay-mynewt/hw/bsp/lt213a/src/hal_bsp.c) 的 `while (!NRF_CLOCK->EVENTS_DONE) {}`。DHCSR 睡眠观测 0/1,339。RAM 中 `g_os_time=0`，屏幕任务结构尚未初始化。
3. 再执行软件 reset 后，采样约 12 秒：804/804 命中 `os_tick_idle` 的 `WFI` (`0x14084`)，睡眠观测 804/804；屏幕任务已初始化，但该 RAM 快照中系统 tick 仍为 0。
4. 这次重启后的 180 秒观察期间没有标签广播，扫描器收到 3,394 次其他广播回调。期间包含上述调试采样和一次 RAM 暂停转储，因此不是完全无调试干预的三分钟稳定性试验。

两种状态必须分别解释：第一次是校准忙等；第二次是已经进入 idle 但没有观察到业务推进。不能把第二次 100% 睡眠样本作为省电优势，也不能根据首次的 100% 忙等推断 Mynewt 正常工作时的 CPU 占用。

没有强行写入校准完成标志、跳过启动逻辑或修改固件来制造可运行的对照。当前证据不足以确定校准时序、软复位后的外围状态或定时器事件为何异常。没有执行真正的断电冷启动，也没有对看门狗进行故障注入；不能由这次观测断言硬件看门狗失效。

## 初次测试的后续建议与复现（故障已在上节修复）

先在确认供电方式后进行一次完整断电冷启动，保持 Mynewt 原二进制，验证能否进入正常广播。若可运行，再复用同一脚本补齐三组各 3 次的负载与工作后 RAM；若仍不运行，应先修复启动/时钟事件，再进行 RTOS 性能排名。

所有原始产物位于本机 [`build/profiling-20260912/`](../build/profiling-20260912/)（构建目录不纳入 Git）：

- `bench.py`：固定图片、官方 SDK、命令响应时间分解与 JSON 轨迹；`--repeats 3`。
- `sample.py`：OpenOCD 运行中 PCSR/DHCSR 采样，`--dump` 在结束时暂停转储 RAM。
- `analyze.py` / `summary.json`：ELF 地址归属、采样分布、栈前缀和时间摘要。
- `zephyr-bench.json`、`zephyr-sampled-bench.json`：分别为正式无调试会话基准和另一次采样验证负载。
- `mynewt-attempt1-bench.json`、`mynewt-*-samples.json`、扫描日志：失败状态原始证据。
- 两个冻结的 ELF/HEX/BIN；`original-flash.bin` 和恢复后的配置快照供本地恢复审计，可能含设备配置，不应作为公开报告附件。

典型命令（在 Zephyr 仓库根目录、对应固件已写入且正常广播时执行）：

```sh
.venv/bin/python build/profiling-20260912/bench.py mynewt DEVICE_UUID --repeats 3
.venv/bin/python build/profiling-20260912/sample.py mynewt-postwork --seconds 20 --dump
.venv/bin/python build/profiling-20260912/analyze.py
```

不同测试标签会生成不同文件名；重复使用同一标签会覆盖该次输出。`sample.py` 独占调试器，正常时延基准不要与其并行运行。

后续延迟与 SPI 优化见 [性能优化实测](performance-optimization-2026-09-12.md)，包含分步对照、最终构建与验证限制。
