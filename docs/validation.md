# 验证指南与证据边界

[文档导航](README.md)

验证分为主机逻辑、固件构建和真实设备三个层次。下面的实机结论来自已有记录，文档整理本身没有重新执行它们；选测试时同时考虑修改涉及的路径、设备副作用和当前构建前提。

Zephyr 4.4.2 升级仅完成构建与主机验证；既有无线、时序和栈实测来自升级前固件，不能作为新版紧凑蓝牙缓冲的实机证据。

## 日常回归

```sh
uv sync --locked
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

已有依赖时省略 `--setup`。主机测试需要支持 ASan/UBSan 的原生 C 编译器；可用 `CC` 指定。测试入口生成压缩流和安全配置 fixture，再编译/运行 C 与 Python 测试，不使用通用 pytest 自动发现来替代这个顺序。

| 变更 | 最小验证范围 |
| --- | --- |
| 固件协议、配置、安全、驱动 | 主机完整测试 + ARM 构建/内存检查；实机覆盖按受影响行为选择 |
| BLE 生命周期、缓冲、时钟 | 上述检查 + 目标客户端的连接/上传/恢复；主机不能模拟射频与完整 Zephyr 调度 |
| 屏幕休眠或构建参数 | 另构建 `--no-epd-deep-sleep`；保留两套产物用于比较 |
| Python 工具 | 主机测试，必要时在指定设备验证备份、写入及回读 |
| 纯文档 | 路径、命令、事实和跨文档一致性 |

构建自动检查加载区域、RAM 初始化镜像、向量表与初始 SP/reset。两种屏幕构建共用 `build/`；测试、烧录和栈分析使用的 ELF 必须匹配设备二进制。

## 主机测试能证明什么

| 入口 | 主要覆盖 |
| --- | --- |
| `tests/test_protocol.c` | raw/zlib、畸形包、长度/校验、PIPE 位图/去重/回绕/结束顺序、区域与 etag |
| `tests/test_storage.c` | fake Flash 事务、写入失败、各阶段中断、重启选择完整槽 |
| `tests/test_security.py` | C 实现与独立密码学原语、KDF/CMAC/CCM、篡改、重放、超时、限速 |
| `scripts/test_epd.py` + `tests/test_epd.c` | 真实驱动的虚拟 GPIO/SPI、LUT、平面、full/fast 一致、两种休眠与超时 |
| `scripts/test_telemetry.py` + `tests/test_telemetry.c` | 温度/VDD 编码、旧快照保留、ADC 停止与超时 |
| `tests/test_upload.py`、`test_serial.py`、`test_partial_frames.py` | SDK 接口、工具输入、扫描状态、备份/保留字段和回读 |

现有完整入口包含 20,000 个畸形协议包和 220 个模拟 Flash 中断点。数字描述测试负载，不能转化成覆盖率或真实掉电保证。密码学测试依赖入口先生成的安全配置，单跑前须满足此条件。

## 实机测试怎么选

所有脚本从仓库根目录执行，`DEVICE` 替换为实际扫描地址；它们不属于日常主机测试。读脚本前提后再运行，恢复配置的 finally 也不能保证断电或失联时一定恢复成功。

| 脚本 | 前提与副作用 | 目标 |
| --- | --- | --- |
| `tests/test_upload_hardware.py DEVICE IMAGE --cycles 2` | 原生 104×212 图片、未认证连接；改屏幕 | 原版 SDK 默认包长、重复 raw/zlib 上传和刷新结果 |
| `tests/test_alignment_hardware.py DEVICE` | BLE + SWD、默认 133 B 无认证配置、另备份 Flash；改屏幕/配置并复位，结束恢复容器 | PIPE 边界与配置事务；正常回归不加 `--baseline` |
| 上一脚本加 `--fast-only` | 已可连接；改屏幕，不改配置 | 显式 END 的 full/fast 时序 |
| `tests/test_partial_hardware.py DEVICE BEFORE AFTER` | 两张原生图片且差异区域小；先全刷再局刷 | 标准局刷或 `--white-first` / `--invert-first` 对照 |
| `tests/test_partial_frames_hardware.py DEVICE` | BLE + SWD、无认证、支持帧数扩展；临时改配置、复位并留测试图 | 100/160 帧时序与持久化，结束恢复配置 |
| `tests/test_security_hardware.py DEVICE BEFORE AFTER` | 无认证原配置、原生图片、SWD 可恢复；临时设 key、刷新，结束恢复 | 真 SDK 错误分支、加密图像、重放与绝对期限 |
| `tests/test_identity_hardware.py DEVICE` | probe-rs + SWD、已知原 BLE 身份；三次复位 | 身份跨复位保持、配置和版本可读 |
| `tests/test_recovery_hardware.py DEVICE` | probe-rs、ARM GDB、精确匹配 ELF及至少 16 B 尾部空闲 RAM；注入死循环约三分钟 | 硬件看门狗恢复 |

调用示例：

```sh
uv run --locked python tests/test_upload_hardware.py DEVICE native-image.png --cycles 2
uv run --locked python tests/test_alignment_hardware.py DEVICE --fast-only
```

**恢复脚本有资源前提**：它使用 `0x20003ff0` 起的末尾 16 B，并断言 ELF RAM 末端不超过该地址。旧版 16,376 B 静态 RAM 样本只剩 8 B，会被拒绝；4.4.2 默认构建样本剩 212 B，满足空间前提，但尚未执行新版实机注入。运行前仍须检查实际 ELF，保留脚本边界断言。

## 已有证据与尚缺范围

| 领域 | 已有证据 | 不能据此声称 |
| --- | --- | --- |
| BLE 大包 | SDK 7.14.1、已测 Mac 协商 MTU 247，默认 raw/zlib 重连上传完成 | 任意 peer 都得到 247，扫描始终可靠 |
| PIPE | 2026-09-16 实机 394 小包跨序号回绕、重复发送、自动结束、失败 END、fatal 后恢复 | 空口真实丢包覆盖、多包乱序吞吐 |
| 配置 | 133/423 B 容器回读、坏 CRC 拒绝、提交后复位、未完成写入复位/断连、clear、恢复原配置 | Flash 擦写电气瞬间掉电原子性、所有字段运行时生效 |
| 认证 | 无/错 key、坏 tag、重放、加密 raw/zlib/局刷、60 秒绝对期限、恢复无 key 配置 | 完整换 key 场景、任意低 MTU、最坏栈安全 |
| 局刷 | 特定板正反向数字更新、帧数持久化；100 帧轻微残影，先白策略较干净 | 无残影、全温区、所有面板质量一致 |
| 全屏 fast | 主机 SPI 序列相同；实机 full/fast 接近的时长 | 已实现独立 fast 波形或光学加速 |
| 名称 | 录入/清空/复位保持、扫描响应编码；同时存在扫描丢名称记录 | 空扫描证明未设置或不在线 |
| 稳定性 | 旧构建硬件看门狗注入通过；一次空闲断连/重连；特定加密负载栈采样 | 当前全部路径最坏水位、长期独立供电可靠性 |
| 功耗 | 事件等待、关电/深睡和遥测逻辑及主机回归 | 实测待机电流、刷新电荷或电池续航 |

## 报告应能让别人复核

记录芯片/面板、供电、工具链、源码提交及未提交差异、二进制哈希、匹配 ELF、客户端/OS、MTU、输入图片、重复次数和失败。计时明确起终点，画质结论注明目视/仪器证据，栈结论注明负载与复位状态。

烧录 ELF 后校验其 Flash PT_LOAD 段；不要把整个 Flash 回读哈希直接当成 BIN 文件哈希，二者可能包含不同的链接对齐填充。2026-09-16 的 PIPE 回归就遇到过 4 B 的 `00/ff` 填充差异，加载段与有效配置分别核对后才确认一致。

原配置备份可能含密钥和设备身份，保存在忽略目录；公开报告只保留必要且已脱敏的信息。配置恢复应比较容器内容，不能要求事务槽历史完全逐字节不变。发布要求见 [CONTRIBUTING](../CONTRIBUTING.md)。
