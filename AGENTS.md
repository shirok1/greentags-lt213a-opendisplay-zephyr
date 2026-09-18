# Agent 工作指南

## 开始工作

这是 Greentags LT213A 的 OpenDisplay 兼容固件：Zephyr C 应用与 Python 主机工具。
先读 [CONTRIBUTING.md](CONTRIBUTING.md) 的硬件边界、依赖和交付约定；环境准备与设备操作见
[README.md](README.md)。技术资料总入口为 [docs/README.md](docs/README.md)；按下面任务索引读取，无需一次加载全部文档。
搜索默认聚焦 `src/`、`scripts/`、`tests/` 和 `boards/`；调查依赖实现时再进入 `.deps/`。

## 修改入口

| 修改内容 | 实现与验证入口 |
| --- | --- |
| BLE、连接生命周期、命令队列、广告、看门狗 | `src/main.c`；`prj.conf` |
| 命令、PIPE、raw/zlib 流、传输恢复 | `src/protocol.c`、`src/protocol.h`；`tests/test_protocol.c` |
| 配置容器、CRC、双槽事务、名称与局刷参数 | `src/config_store.c`；`tests/test_storage.c`、`tests/test_protocol.c` |
| Zephyr Flash 适配与分区 | `src/storage.c`；`boards/greentags/lt213a/lt213a_nrf51822.dts` |
| 认证、加密、会话与重放窗口 | `src/security.c`、`src/crypto.c`、`src/crypto_zephyr.c`；`tests/test_security.py`、`tests/security_bridge.c` |
| 屏幕时序、波形、休眠 | `src/epd.c`；`scripts/test_epd.py`、`tests/test_epd.c` |
| 温度、VDD 与广播遥测 | `src/telemetry.c`；`scripts/test_telemetry.py`、`tests/test_telemetry.c` |
| 上传、序列号、局刷帧数工具 | `scripts/upload.py`、`scripts/set_serial.py`、`scripts/set_partial_frames.py`；对应 `tests/test_*.py` |

协议通过 `struct od_io` 接入设备操作，配置事务通过 `struct od_flash_ops` 接入 Flash。
保持这些主机可测边界；Zephyr 设备调用留在适配层。BLE 写回调负责入队，耗时的协议与屏幕操作
由主循环处理；修改连接逻辑时保留 connection 引用管理和 generation 检查，避免旧连接命令作用于新连接。

## 构建与验证

以下命令均在仓库根目录执行：

```sh
uv sync --locked
# 首次准备依赖并构建；已有依赖的日常构建省略 --setup
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

- 使用 `scripts/build.py` 作为构建入口：它配置工具链、应用 `scripts/patch_zephyr.py` 的固定版本
  缓冲补丁并自动检查 ELF 内存边界与复位向量。
  Zephyr 使用正式 tag，HAL/CMSIS 版本通过 manifest import 继承；仅额外依赖单独固定。
- 固件变更执行构建和主机测试，报告本次 Flash/RAM 结果；更换工具链时加 `--pristine`。
  屏幕休眠或构建配置变更还需构建 `--no-epd-deep-sleep` 对照版本，CI 流程见
  [.github/workflows](.github/workflows)。两种构建共用 `build/`，后一次会覆盖前一次产物。
- 主机测试统一入口是 `scripts/test.py`，它生成共享 fixture 后运行 C sanitizer 和 Python 测试。
  `tests/test_security.py` 依赖该入口生成的配置；AES 主机后端使用 uv 管理的 cryptography，不能当作完全独立的测试运行。
- Python 工具变更运行主机测试；纯文档变更核对路径、命令和事实即可。
- `tests/*_hardware.py` 是实机脚本，可能写入配置、刷新屏幕或注入故障；仅在任务包含对应设备操作时执行，
  先读脚本前提并确定目标设备。烧录命令中的 mass erase 会清除持久配置，具体流程见 README。
- 完成时说明实际执行的验证和未验证部分；主机测试不证明无线、电气、功耗或实机时序正确。

## 资源与生成文件

- 本板只有 128 KiB Flash / 16 KiB RAM；应用可用 126 KiB Flash，末尾两个 1 KiB 页用于配置事务。
  内存余量以当前构建的 `scripts/check_memory.py` 输出为准，文档中的历史数字不能替代本次测量。
- 保持流式图像处理。新增缓冲区、线程栈、队列深度或日志前评估 RAM；静态区域之外的剩余 RAM
  与线程栈内部余量是两个指标，构建通过也不代表最坏情况下栈安全。
- 默认配置由 `scripts/generate_config.py` 生成到已跟踪的 `src/config.inc`。
  修改默认值后运行 `uv run --locked python scripts/generate_config.py`，并执行主机测试核对容器字节与 CRC。
- Python 与固件依赖分别以 `uv.lock`、`west.yml` 为准；生成目录、密钥和设备配置备份的提交边界见 CONTRIBUTING。

## 按任务读取资料

| 任务 | 先读 |
| --- | --- |
| 安装、烧录、上传与工具使用 | [使用指南](docs/guide.zh-CN.md) |
| 理解硬件、请求路径与模块边界 | [架构](docs/architecture.md) |
| OpenDisplay、PIPE、配置事务与字段生效范围 | [协议](docs/protocol.md) |
| 认证、加密、会话与换 key | [安全](docs/security.md) |
| 扫描、身份、序列号、MTU 与接收问题 | [BLE](docs/ble.md) |
| T5 波形、局刷帧数、残影、full/fast | [屏幕](docs/display.md) |
| 广播、遥测、看门狗与功耗 | [电源](docs/power.md) |
| 内存、栈、编译参数与性能测量 | [性能](docs/performance.md) |
| 选择测试、判断实机证据与运行前提 | [验证](docs/validation.md) |

行为变化时更新对应主题及受影响的 README 摘要。新增实测保留固件版本、条件、结果和限制，
将结论融入主题；不要追加“本轮完成/后续修复”的任务流水账。实验数字集中在专题文档，
本入口只保留导航与工作约束。
