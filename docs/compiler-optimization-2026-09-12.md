# 编译参数优化实验（2026-09-12）

> 后续系统裁剪已使两边全局 `-O2` 可用并成为默认；最新结果见 [系统裁剪与全局性能](system-trimming-2026-09-12.md)。以下保留裁剪前实验数据。

## 本阶段采用的配置

Zephyr 仅 `src/epd.c` 使用 `-O2`，其余维持 `-Os + LTO`，配置在根目录 `CMakeLists.txt`。实际命令中该文件为 `-Os -flto ... -O2`，最后一个优化等级生效。生产构建 BIN 与已完成实板测试的候选逐字节一致。Mynewt 默认编译参数保持原值；所有临时依赖修改已恢复，默认构建 BIN 与上轮基线逐字节一致。

工具链为 Arm GNU Toolchain 15.3.Rel1，GCC 15.3.1 20260627，Cortex-M0。优化等级和 LTO 行为依据 [GCC 官方选项说明](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)，选型以本机实际构建和板上结果为准。

## 构建筛选

| 候选 | 构建结果 / 决策 |
|---|---|
| Zephyr 全局 `-O2 + LTO` | Flash 超限 5,984 B，淘汰 |
| Zephyr 全局 `-O3 + LTO` | Flash 超限 52,132 B，并有运行库符号链接错误，淘汰 |
| 两边全局 `-Oz` | 分别与各自同身份 `-Os` 基线 BIN 完全相同，无收益 |
| Zephyr EPD + 解压 `-O2` | Flash 100,452 B、RAM 16,360 B；实板通过，额外解压优化收益不足 |
| **Zephyr 仅 EPD `-O2`** | **Flash 99,228 B、RAM 16,360 B；采用** |
| Mynewt 全局 `-O2` | 链接器检查：运行时 heap 小于 2 KiB；未放宽检查 |
| Mynewt 全局 `-Os/-O2 + LTO` | 普通 ar 缺少 LTO plugin；换 gcc-ar 后仍有运行库符号错误，未采用 |
| Mynewt 仅 uzlib LTO | 测试地址 Flash 93,160 B，比同身份基线增加 16 B；未上板，未采用 |
| Mynewt 应用 + uzlib + nrf_common 独立 LTO profile | 补齐链接处理后实板通过；收益小、集成成本高，不改默认 |

Mynewt 的应用 `pkg.cflags` 会成为全局基础 flags，不能据此实现应用局部优化。真正局部实验用三包的 `pkg.build_profile: lt213a_lto` 与编译器 profile `[compiler.flags.default, -Os, -flto, -ggdb]`；归档工具为 `arm-none-eabi-gcc-ar`，链接需显式保留 `__aeabi_llsr`（`-Wl,-u,__aeabi_llsr`）。仅加链接 `-flto` 不能解决此符号错误。最终测试链接同时含 `-flto` 和该符号保留参数。它的测试地址 Flash 为 92,596 B，相比同身份基线 93,144 B 少 548 B；静态 RAM 增加 8 B，运行时 heap 从 2,064 B 降到 2,056 B。

## 实板时间

同一标签、CMSIS-DAP、py-opendisplay 7.14.1、MTU 247、PIPE window/ACK interval 2。复用上轮 2,756 B 固定图与局刷窗口。每类各 3 次，另一次全刷建立局刷基线。表中均为中位数，单位秒；START/END 计时包含主机、BLE、调度和面板初始化，并非纯 CPU 周期。

| 构建 | 原始全刷 | 压缩全刷 | 局刷 | 原始 START ACK | 原始到 END ACK |
|---|---:|---:|---:|---:|---:|
| opt-zephyr-spi | 4.302 | 3.915 | 2.352 | 0.241 | 0.869 |
| compiler-zephyr-hot-o2 | 4.031 | 3.974 | 2.337 | 0.133 | 0.597 |
| compiler-zephyr-epd-o2 | 4.003 | 4.015 | 2.308 | 0.135 | 0.569 |
| compiler-zephyr-baseline-repeat | 4.093 | 3.987 | 2.351 | 0.242 | 0.903 |
| opt-mynewt-spi | 4.514 | 4.632 | 2.499 | 0.463 | 1.065 |
| compiler-mynewt-local-lto | 4.438 | 4.586 | 2.457 | 0.420 | 1.020 |

Zephyr 原参数在优化前后两次测量中，START ACK 都约 0.24 s；仅 EPD `-O2` 约 0.135 s。这一收益比总时长稳定。完整刷新阶段出现约 0.3 s 波动，压缩总时长并非每轮都下降，因此不宣传统一百分比的整屏加速。Mynewt 局部 LTO 在本次样本中只改善约 1–2% 的总时长，不足以支持增加默认构建复杂度。

## 资源与验证边界

- Zephyr 最终相比上轮增加 2,896 B Flash、24 B 静态 RAM。剩余 24 B 是预留栈/缓冲区之外的 RAM，并非任务栈剩余量。
- 候选和回测共 40 次刷新收到 `00 73`；Mynewt LTO 另通过连续 3 次软件复位。计时期间没有调试会话。
- 最终 Zephyr 构建、内存布局检查及完整主机测试通过。未改变 SPI 最小半周期、BUSY 延迟、100 帧 LUT 或 BLE 参数。
- 没有全温区、低电压和整板电流测量；也没有对本轮候选做实板加密会话验证。Mynewt LTO 仅作为实验结果保存。

## 产物与复现

本机 `build/compiler-opt-20260912/` 保存候选 ELF、失败日志、编译/链接命令、矩阵脚本、`summary.json` 和备份。原始轨迹是 `build/profiling-20260912/compiler-*-bench.json`。临时 Mynewt profile/归档工具已恢复，不应直接运行残留中间产物作为默认构建。

Zephyr 正常运行 `scripts/build.py` 即应用选定参数，无需实验 CMake hook。最终 BIN SHA-256：`4145103b35c9d45d1fa5176d53aa8035a03f2c865c2ea5b57f482deb22564051`。Mynewt 默认 BIN SHA-256：`3f0622c1fe625e0e20d9a0318c3852dedd390db1829094030735c205deccd7c3`。备份可能包含配置数据，不应公开。

最终 Zephyr 另完成一次明文压缩全刷，版本、遥测均读取成功，屏幕恢复 `OPT / 1`。随后暂停转储 RAM：main/workqueue/RX/priority RX/idle 栈未使用前缀为 572/380/564/196/84 B，线程哨兵完整；ISR 未使用前缀 536 B（不设哨兵）。此水位仅覆盖最终复位后的配置读取和一次明文全刷，不覆盖实板加密或最坏中断嵌套。OpenOCD 转储结束附近记录 external reset，时延基准不包含此次暂停。最后 10 KiB 配置与本轮开始前备份一致。
