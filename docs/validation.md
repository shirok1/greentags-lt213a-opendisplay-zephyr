# 验证指南

[文档导航](README.md)

主机测试验证协议和驱动逻辑，ARM构建检查目标资源，实机测试验证无线与设备行为。
三者不能互相替代；测试范围应与修改涉及的路径相匹配。

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
| `tests/test_notify.py` + `tests/test_notify.c` | 实际发送函数的超时占槽、连接世代、迟到回调、重试和回绕；Zephyr 调度接口用确定性替身 |
| `tests/test_zlib.py` | 500 随机流、块类型交替、单字节输入输出、奇数 HLIT 和 15-bit 码 |
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
| `tests/test_security_hardware.py DEVICE BEFORE AFTER` | 无认证原配置、原生图片、SWD 可恢复；临时设 key、刷新，结束恢复 | 真 SDK 错误分支、加密图像、并发描述符/配置读取、通知中断连、重放与绝对期限 |
| `tests/test_identity_hardware.py DEVICE` | probe-rs + SWD、已知原 BLE 身份；三次复位 | 身份跨复位保持、配置和版本可读 |
| `tests/test_recovery_hardware.py DEVICE` | probe-rs、OpenOCD、精确匹配 ELF及至少 16 B 尾部空闲 RAM；注入死循环约三分钟 | 硬件看门狗恢复 |

调用示例：

```sh
uv run --locked python tests/test_upload_hardware.py DEVICE native-image.png --cycles 2
uv run --locked python tests/test_alignment_hardware.py DEVICE --fast-only
```

**恢复脚本有资源前提**：它使用 `0x20003ff0` 起的末尾 16 B，并断言 ELF RAM 末端不超过该地址。旧版 16,376 B 静态 RAM 样本只剩 8 B，会被拒绝；参考默认构建剩156 B，满足空间前提。运行前仍须检查实际 ELF，保留脚本边界断言。

## 已验证范围与限制

当前BLE/解码/安全实现的同板回归覆盖MTU247、加密raw/zlib/局刷、错误key/tag、
重放与绝对期限、五次加密配置和40次并发描述符读取、三次通知中途断连重认证。
结束后原配置经重新连接回读确认。负载与构建标识见 [性能](performance.md)。

| 领域 | 已有覆盖 | 尚不能证明 |
| --- | --- | --- |
| BLE | 目标Mac连接、配置读取、上传与重连 | 任意peer、持续压力、扫描始终可靠 |
| PIPE与配置 | 实机序号回绕/重传、CRC拒绝、事务中断与恢复；主机故障注入 | 空口真实丢包、物理Flash掉电原子性 |
| 认证 | 错误分支、加密图像、并发读取、断连重认证、60秒绝对期限 | 全部换key场景、低MTU互通、最坏栈 |
| 屏幕 | 驱动序列、全刷/局刷完成、帧数生效 | 当前版本光学质量、全温/低压、长期残影 |
| 稳定性 | 三ATT修复版通过看门狗注入与BLE恢复；当前实现有栈水位样本 | 后续所有布局的故障组合、长期独立供电可靠性 |
| 功耗 | 事件等待、关电/深睡和遥测逻辑 | 待机电流、刷新电荷、电池续航 |

扫描存在偶发超时：已有现场快照显示控制器广告开启，仍不足以定位问题在固件、空口
还是macOS。一次成功重连不代表该问题已解决。详见 [BLE排查](ble.md)。

## 测试前提与恢复

安全脚本会短暂启用随机key，须保持SWD恢复通道，并保护私有备份。密集认证测试需
遵守固件限速；脚本在后续图像阶段前等待窗口结束，不改变固件的10次/分钟限制。

SDK可将恢复配置的容器长度写为协议允许的0，而部分配置实机测试要求默认非零长度。
串联测试前应核对容器长度、正文和CRC，避免将合法编码差异误报为Flash损坏。配置
恢复比较有效容器内容，不要求事务槽历史完全逐字节相同。

故障注入后应在看门狗复位前结束调试会话。复位后再halt可能干扰正常无线时序，不能
将调试造成的扫描失败作为原有故障。RAM采样也必须标明halt/reset时机和匹配ELF。

## 可复核的验证报告

记录芯片/面板、供电、工具链、源码版本及差异、二进制哈希、客户端/OS、MTU、输入、
重复次数、失败和恢复结果。计时写明起终点；画质写明目视或仪器证据。构建时Git SHA
不能独自标识未提交固件，也不能把版本号变更后的二进制称为已经重新实机验收。

烧录后校验ELF加载段；整个Flash回读与BIN可能因填充和配置页不同而有不同哈希。
备份、密钥、RAM转储和设备身份留在忽略目录，公开文档只保留必要且脱敏的结论。
发布要求见 [CONTRIBUTING](../CONTRIBUTING.md)。
