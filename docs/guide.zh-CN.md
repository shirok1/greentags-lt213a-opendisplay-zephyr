# LT213A 中文使用与实现说明

[English / 项目首页](../README.md) · [贡献说明](../CONTRIBUTING.md)

目标为 nRF51822 QFAB，**128 KiB Flash / 16 KiB RAM**。使用固定版本的
Zephyr 3.7.1 BLE Host + 软件 Link Layer，**不使用 SoftDevice**。
Python 环境由 `uv + pyproject.toml + uv.lock` 管理。构建使用 Zephyr 原生 west/CMake，
不引入 PlatformIO，以便直接控制板级配置、BLE 缓冲区及内存预算。

## 硬件

| 信号 | GPIO | 配置 |
|---|---|---|
| MOSI / SDA | P0.30 | 输出，MSB first |
| SCK | P0.00 | 软件 SPI mode 0 |
| CS | P0.01 | 低有效 |
| DC | P0.02 | 低命令、高数据 |
| RESET | P0.03 | 低有效 |
| BUSY | P0.04 | 低忙、高就绪，无内部上拉 |
| BS | P0.05 | 固定低，四线 SPI |

本项目针对的板卡配置：没有外置 Flash、LED、蜂鸣器、NFC、电源锁存或唤醒按键。
使用内部低频 RC 时钟并校准；P0.00 不作为低频晶振引脚。
没有电池采样电路，MSD 电池字段为 0，温度来自片内传感器。

## 构建与测试

安装 uv、Git、CMake、Ninja，以及 ARM GCC 或 Zephyr SDK：

```sh
uv sync --locked
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

`--setup` 获取 `west.yml` 固定提交的 Zephyr、CMSIS、Nordic HAL 和 TinyCrypt。
这些依赖缓存在 `.deps/`。uv 自动管理 `.venv`，不需要手动建环境或运行 pip。
后续构建省略 `--setup`；干净重建使用 `--pristine`。
本地验证工具链为 Arm GNU 15.3.Rel1、CMake 4.4.3，启用 LTO。

产物在 `build/zephyr/`：`zephyr.hex`、`zephyr.bin`、`zephyr.elf`、`zephyr.map`。
构建脚本检查 ELF 加载地址、向量表及 Flash/RAM 边界。

Flash 前 126 KiB 为程序，末尾两个 1 KiB 页为配置事务槽。
默认省电配置链接结果 **92,956 B Flash、16,168 B RAM**，以构建脚本最终输出为准。
RAM 已含静态线程栈和 BLE 缓冲区，未分配空间仅 216 B；这不是栈余量。
启用栈填充、栈哨兵及栈信息，最坏调用深度仍需真机测量。
不分配完整 MCU 帧缓冲，也不使用动态堆。

## 协议覆盖

基准为 [OpenDisplay 官方协议](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/include/opendisplay_protocol.h)
及同提交的配置结构。服务及特征 UUID 均为
`00002446-0000-1000-8000-00805f9b34fb`，公司 ID 为 `0x2446`。
先订阅通知，再发送命令。

| 命令/能力 | 实现及限制 |
|---|---|
| `0x40` 配置读取 | 编号分片、总长度、容器 CRC16 |
| `0x41/0x42` 配置写入/续传 | 最大 768 B；校验结构、硬件约束和 CRC 后提交 |
| `0x45` 清除配置 | 事务提交恢复默认配置，清除认证配置 |
| `0x43/0x44` | 固件版本 0.2.0（含构建提交 SHA）、16 B MSD |
| `0x50` 认证 | AES-CMAC 双向认证、会话密钥派生、AES-CCM 加密、重放窗口、超时及限速 |
| `0x70/0x71/0x72` | 原始或 zlib 流式写入、结束和刷新 |
| `0x73/0x74` | 刷新成功/失败通知 |
| `0x76` 局部传输 | etag、区域校验、旧/新像素平面、可选压缩 |
| `0x80/0x81/0x82` PIPE | 协商、序号、SACK、重复包去重、序号回绕、原始/压缩及局部传输 |
| `0x0f` | 中止传输并复位 |
| `0x52/0x53` 关机/深睡 | 返回不支持；没有锁存及唤醒硬件 |
| LED、蜂鸣器、NFC | 返回不支持/对应失败码；配置不声明这些器件 |
| DFU/OTA | 不支持；当前布局没有第二镜像槽和引导程序，更新走 SWD |

这是针对该板硬件的实现，**不是所有 OpenDisplay 可选硬件功能均可用**。
内部剩余 Flash 无法容纳第二份当前固件；没有实现不可恢复的原地 OTA 擦写。
上游 MCU 枚举没有 nRF51822，使用 `0xffff`，部分客户端可能显示 Unknown。

### 分片与流控

存在三层分片：配置的应用层编号分片、图像 DATA/PIPE 分块、BLE 协议栈的
L2CAP/Link Layer 分片。协商 ATT MTU 最大 247；nRF51 使用 1M PHY、27 B 链路层 PDU。
普通 DATA 最大 230 B；默认 MTU 23 时可发送 18 B 原始数据。
认证后存在 29 B 加密开销，需要更大 MTU，客户端必须按实际 MTU 缩小数据块。

PIPE **协商窗口 W=1、确认间隔 N=1**，客户端必须遵守协商值。
每块确认，SACK 包含此前 32 块状态；重复包不重复写屏，错误序号终止会话。
不缓存乱序图像。zlib 解压使用 512 B 窗口，流式写屏并验证 Adler32、输出长度及结束标记。

全屏数据为 104×212 单色、2,756 B，13 B/行，高位优先，0 黑、1 白。
局部数据包含旧/新两个平面，X 和宽度按 8 像素对齐。etag 仅在刷新成功后更新，
保存在 RAM；重启后必须先完整上传。整屏 fast 请求目前使用完整刷新波形。
局部刷新现使用用户提供的 GDEW0213T5 Arduino P20201021 示例参数及 LUT；
局部写入时由驱动反转旧/新像素极性，客户端保持标准 OpenDisplay 像素格式。
示例建议每五次局部刷新后进行一次全刷清除残影，当前不自动计数强制全刷。
已完成一次 PIPE 局部更新实测（32×15 像素，46 B，约 1.15 秒），最初出现旧数字未擦除；统一 LUT 阶段为 100 帧后完整数字已可更新（约 2.88 秒）；160 帧约 4.08 秒、更干净；最新测试采用局部白 100 帧再目标 100 帧（共约 5.77 秒），用户确认更干净、笔画正常，当前固件为每次 100 帧；见 [实测记录](partial-t5.md)。

传输不完整、溢出、压缩损坏和非法状态均拒绝。断线、30 秒传输空闲或发送失败时中止。
队列满时断开连接，避免静默丢包。BUSY 等待有 30 秒上限，刷新在主线程执行。

### 配置和认证

支持系统、制造商、电源、显示、安全及扩展文本配置块；配置必须与上述硬件匹配。
不允许通过远程配置更改接线或声明不存在的外设。默认配置由
`scripts/generate_config.py` 生成，默认不开启认证。
启用认证时需写入有效安全配置和非零 128 位密钥，安全重置/显示/重写标志必须为 0。
认证算法与独立 Python/OpenSSL 实现交叉验证；官方 Python 包实机握手、加密配置/全刷/局刷、错误处理和绝对会话期限已通过，见 [认证互通记录](auth-interoperability.md)。

配置直接写入非活动 Flash 页，CRC 验证后最后写提交标记，断电恢复选择最新有效槽。
清除配置不等于安全擦除历史密钥；旧槽可能保留旧内容，直到被下一次事务覆盖。

## SWD 与上传工具

使用 J-Link，连接 SWDIO、SWCLK、GND 和目标电压参考。以下命令整片擦除，
包括已保存配置；需要原厂固件时先备份。本项目已通过 CMSIS-DAP / probe-rs 烧录并验证全屏刷图。

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
uv run --locked west flash -d build -r jlink --erase --reset
uv run --locked python scripts/upload.py scan
uv run --locked python scripts/upload.py upload '扫描得到的地址'
uv run --locked python scripts/upload.py upload '扫描得到的地址' picture.png --compress
```

macOS 地址为 peripheral UUID。图片等比缩放补白至 104×212 并转单色；不提供图片时
上传方向/棋盘测试图。此辅助工具使用 `py-opendisplay` 7.14.1（兼容 Python 3.11/3.12）
完成扫描、协议收发、压缩及刷新确认，支持未认证的普通/压缩上传。
使用包的默认 PIPE 选择与窗口设置；固件协商 W=N=1。原始全帧自动结束，压缩及局部传输等待显式 END。
固件启用 nRF51 明文 DLE 支持，保持 27 B 链路包长；本机 macOS 实测 ATT MTU 247。
直接使用包的默认 230 B DATA 数据块和 200 B START 包，不修改客户端常量。
详见 [大包调查和实机结果](ble-large-packets.md)。
PIPE、配置和认证可通过符合上述协议的客户端使用，辅助工具没有这些命令界面。

## 验证与待测项

主机测试涵盖原始分块、动态/固定/无压缩 DEFLATE、逐字节压缩输入、损坏校验和、
PIPE 序号回绕和重传、局部区域和 etag、配置提交/恢复、220 个模拟断电位置、
20,000 个畸形协议包，以及认证/KDF/CCM/重放/限速/超时。
C 协议和存储测试使用 ASan/UBSan，加密测试使用 UBSan 和独立 OpenSSL 基元。

已完成官方 Python 包版本读取及默认 PIPE 普通/压缩全屏连续上传。
已完成认证/加密及配置恢复实机回归，局部先白100帧再目标100帧已由实屏确认。
当前实测栈余量为主线程252 B、系统工作队列380 B，总RAM未增加。
尚未完成：实际断电时 Flash 行为、所有路径的最坏栈占用、整板电流和长时间独立供电稳定性。

整屏初始化参考 GDEW0213T5 Arduino 示例（2019-10-16）：`06 17 17 17`、`04`、`00 1f 0d`、
`61 68 00 d4`、`50 97`、旧平面 `10` 填白、新平面 `13`、刷新 `12`、
关电 `50 f7 / 02`。第三方来源与修改见 [NOTICE.md](../NOTICE.md)；许可见 [COPYING](../COPYING)。

## 待机省电

参考官方 nRF 策略：启动广播前 **10 秒使用 160 ms**，随后使用 **1000 ms**
（另有 BLE 随机延时），TX 为 0 dBm。快速阶段期间若已连接，切换推迟到断开；
进入慢速阶段后重连不重新触发快速阶段。扫描建议持续至少 8 秒。
连接连续 **120 秒没有被接受的业务命令**后主动断开；GATT 发现、版本查询、
认证交互、被拒绝的请求和链路保活不延长这个时限。
上传、配置或认证客户端需要在此期间开始操作。图像/配置续传仍使用 30 秒空闲超时；
同步刷新过程不被空闲断开打断，处理结束后重新计算应用活动时间。

主线程由命令、连接变化或最近截止时间唤醒，不再每秒轮询。
一分钟更新一次广播心跳，温度最多每五分钟采样一次，图像传输期间推迟采样。
Zephyr Tickless/RTC 保持启用；无线事件和 RC 校准仍会唤醒芯片，
没有关闭保证 BLE 时钟精度的 RC 校准，也没有进入无法从 BLE 唤醒的 System OFF。

屏幕启动时先 RESET，再执行原厂 Power Off 序列，因此从未上传图片也会尝试关电。
成功关电后重复调用不再发送 SPI；空闲 CS/RESET 为高，SCK/MOSI/DC/BS 为低。
屏幕 BUSY 异常时等待有界，不因启动关电失败而停止 BLE；不能据此保证故障屏幕已经省电。

默认只执行原厂 T5 示例的 Power Off。深睡作为实验选项：

```sh
uv run --locked python scripts/build.py --epd-deep-sleep
```

该选项在 Power Off 成功后发送 `07 A5`，断开 BUSY 输入缓冲；
下一次上传恢复 BUSY 输入并硬件 RESET。需在实际 T5 上验证指令兼容、全屏/局部刷新、
连续唤醒和电流。再次运行不带此选项的构建会恢复默认配置。

新增虚拟 GPIO/时钟测试覆盖两种模式下的启动关电、重复关电、RESET 唤醒、刷新后休眠、
BUSY 超时与恢复，使用真实 `epd.c` 及 ASan/UBSan。它不替代电气及 BLE 实测。
功耗优化尚无实测电流或电池寿命结论；应断开调试器对比上电未上传、刷新后、
已连接空闲、默认关电和实验深睡的电流波形。


### 与官方省电策略的对应

核对基准：OpenDisplay/Firmware 的 `7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb`，
以该固定提交为参考，关键源码：
[BLE nRF](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/ble_transport_nrf.cpp)、
[主循环](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/main.cpp)、
[命令活动计时](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/communication.cpp)、
[屏幕电源会话](https://github.com/OpenDisplay/Firmware/blob/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb/src/display_service.cpp)。

| 官方实现 | 本项目处理 |
|---|---|
| Bluefruit 快/慢广播 160 ms / 1000 ms，启动 fast timeout 10 s | 用 Zephyr 两阶段广播实现；兼顾初始发现延迟与长期广播耗电 |
| 无配置、按钮触发时加速到 20/30 ms | 固定板级默认配置有效且没有按钮，不增加该模式 |
| BLE 业务空闲 120 s 断开；解析命令后检查；刷新结束重置时钟 | 采用该时限与处理顺序，只用被接受的业务命令更新连接活动时间 |
| 屏幕 keep-alive 窗口为零或刷新失败时立即关闭 | 保留立即关电，无 MCU 整帧缓存，不新增 warm-idle 常驻窗口 |
| nRF 使用 Bluefruit/SoftDevice 低功耗与 DCDC | 保留 Zephyr Tickless、事件等待；不使用 SoftDevice；板上 DCDC 外围未确认，不盲目开启 |
| ESP32 定时深睡、电源轨控制及外部 Flash 休眠 | 硬件和平台不匹配，不移植 |
| MSD 更新可能重启快速广播 | Zephyr 原地更新广播数据，保留慢速阶段，避免周期性重新加速 |

此处的策略参考不代表官方固件的功耗数值适用于 nRF51822。
广播间隔是发现延迟与无线耗电的折中，当前值参考官方策略。
BLE 时序、竞争条件和整板电流仍需真机验证。
