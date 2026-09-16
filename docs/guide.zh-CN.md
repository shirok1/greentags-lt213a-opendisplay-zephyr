# 使用指南

[文档导航](README.md) · [English](../README.md)

LT213A 是一块使用 nRF51822 QFAB 和 GDEW0213T5 电子纸的价签。本项目用 Zephyr 的 BLE Host 与软件控制器实现 OpenDisplay 协议，可通过蓝牙上传黑白图片、修改受支持的配置，通过 SWD 更新固件。

它是独立移植，仍属实验项目。官方 Python SDK 的全屏上传、认证和部分局刷路径已有实机验证；整板功耗、真实掉电恢复、全温区画质和最坏栈使用尚未闭合。具体范围见[验证指南](validation.md)。

## 先确认硬件

本固件只面向 **128 KiB Flash / 16 KiB RAM 的 nRF51822 QFAB**，原生画面为 **104×212、1 bit/pixel**。没有外置 Flash、LED、蜂鸣器、NFC、电源锁存或唤醒按键；内部 ADC 测量 VDD 作为电池电压代理。接线和资源布局见[架构](architecture.md)。

本项目不使用 Nordic SoftDevice，也不支持 OTA。修改设备配置不能把固件变成任意屏幕或接线的通用程序。

## 构建和主机测试

安装 Git、uv、CMake、Ninja、原生 C 编译器，以及 `arm-none-eabi-gcc` 或 Zephyr SDK。在仓库根目录运行：

```sh
uv sync --locked
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

Python 使用 3.11 或 3.12，由 uv 管理环境。`--setup` 把固定提交的 Zephyr、CMSIS、Nordic HAL 和 TinyCrypt 下载到 `.deps/`；后续构建省略该参数。更换工具链时用 `--pristine` 重建。

构建入口会应用项目的 Zephyr 补丁并检查 ELF 内存边界。产物位于 `build/zephyr/`：烧录使用 `zephyr.hex`，调试与测量保留匹配的 `zephyr.elf`、`zephyr.map` 和 `zephyr.bin`。以本次构建报告判断空间是否足够。

## 烧录

默认使用 OpenOCD 与 CMSIS-DAP，连接 SWDIO、SWCLK、GND 和目标电压参考；默认 SWD 频率 10 MHz。已有构建仍选择 J-Link 时，先做 pristine 构建。

下面是整片擦除安装流程，**会删除原固件和持久配置，包括序列号与密钥**。需要保留的内容应先备份。构建和主机测试本身不接触设备。

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
uv run --locked west flash -d build --cmd-pre-load "reset halt" --cmd-pre-load "nrf5 mass_erase" --verify
```

OpenOCD 完成校验后复位运行。J-Link 可用 `uv run --locked west flash -d build -r jlink --erase --reset`。保留配置的升级需要明确限制擦写范围；不要把上面的 mass erase 命令用于保留配置的流程。

## 上传图片

```sh
uv run --locked python scripts/upload.py scan
uv run --locked python scripts/upload.py upload 'DEVICE_ADDRESS'
uv run --locked python scripts/upload.py upload 'DEVICE_ADDRESS' picture.png --compress
```

使用扫描得到的地址，macOS 上通常是 peripheral UUID。省略图片上传方向/棋盘测试图；提供图片时等比缩放、补白到 104×212 并转为单色。工具使用官方 SDK 完成压缩、传输和刷新确认，当前上传界面用于未启用认证的设备。

扫描至少覆盖八秒，慢广播或无线环境差时可以更久。未扫描到名称不等于设备未配置，参见 [BLE 排障](ble.md)。刷新完成通知表示驱动完成流程，画面质量仍需目视检查。

## 设置序列号和局刷帧数

```sh
# 扫描后按编号选择设备并录入序列号
uv run --locked python scripts/set_serial.py
# 只列出未设置和状态未知的设备，不写入
uv run --locked python scripts/set_serial.py scan --unset
# 定向录入
uv run --locked python scripts/set_serial.py 'DEVICE_ADDRESS' 2402859c

# 扫描选择设备并设置局刷帧数
uv run --locked python scripts/set_partial_frames.py
uv run --locked python scripts/set_partial_frames.py 'DEVICE_ADDRESS' 160
uv run --locked python scripts/set_partial_frames.py 'DEVICE_ADDRESS' --reset
```

两个工具都会保存原配置、保留其他字段，并在写入后重连验证。认证设备可用 `--key-file PATH` 提供原始 16 字节密钥。序列号的显示截断、扫描状态见 [BLE 与身份](ble.md)；帧数的含义、残影取舍和局刷测试见[屏幕驱动](display.md)。

## 接下来

开发固件先读[架构](architecture.md)，接入其他客户端先读[协议](protocol.md)。需要诊断时，记录固件二进制、客户端版本、硬件型号与可复现步骤，再按[验证指南](validation.md)选择测试；贡献和发布要求见 [CONTRIBUTING](../CONTRIBUTING.md)。
