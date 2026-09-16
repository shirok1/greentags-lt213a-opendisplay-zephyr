# LT213A OpenDisplay · Zephyr

OpenDisplay-compatible firmware for the **Greentags LT213A**, with an
**nRF51822 QFAB (128 KiB Flash / 16 KiB RAM)** and a **GDEW0213T5 (104 × 212)** e-paper panel.
It uses Zephyr 3.7.1's BLE host and software controller, without Nordic SoftDevice.
This is an independent, experimental port, not an official OpenDisplay firmware release.

[中文使用指南](docs/guide.zh-CN.md) · [Technical documentation / 技术文档](docs/README.md) · [Contributing](CONTRIBUTING.md)

## Capabilities and limits

- Raw/zlib streaming, PIPE with a negotiated window of one, persistent configuration,
  application-layer authentication and encryption, and temperature/internal-VDD telemetry.
- Full-screen uploads and selected authenticated and partial-update paths have been tested
  on hardware with py-opendisplay 7.14.1. T5 partial updates can retain slight ghosting;
  full-screen fast mode uses the full-refresh waveform.
- Serial-number naming and configurable partial-drive frames use existing OpenDisplay
  DataExtended fields. Other accepted configuration fields are not all consumed at runtime.
- Fixed wiring and one panel; no external Flash, LED, buzzer, NFC, power latch or wake button
  is assumed. Firmware updates use **SWD**, not OTA.

Whole-board current, physical power-loss recovery, worst-case stack headroom, long-term
standalone reliability and full-temperature optical quality remain unverified. See the
[validation guide](docs/validation.md) for the scope of existing evidence.

## Build and test

Install Git, [uv](https://docs.astral.sh/uv/), CMake, Ninja, a native C compiler with
ASan/UBSan, and `arm-none-eabi-gcc` or a Zephyr SDK. From the repository root:

```sh
uv sync --locked
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

Python 3.11/3.12 dependencies are managed by uv. `--setup` fetches the commits pinned in
[west.yml](west.yml) into `.deps/`; omit it for subsequent builds. Use `--pristine` after
changing toolchains. The build entry point applies the pinned Zephyr patch and checks ELF
Flash/RAM bounds and reset vectors. Outputs are in `build/zephyr/`.

The application owns 126 KiB Flash; the last two 1 KiB pages hold configuration transaction
slots. RAM is tightly constrained, with no dynamic heap or full-frame MCU buffer. Use the
current build report for memory usage; static RAM headroom is distinct from stack headroom.
The default is global O2 + LTO and panel deep sleep; `--no-epd-deep-sleep` builds a power-off
comparison variant into the same output directory. See [performance and memory](docs/performance.md).

## Flash and upload

Connect a CMSIS-DAP probe over SWD and install OpenOCD. The default runner uses 10 MHz SWD.
The following installation command **erases the whole chip, including saved configuration**;
back up anything that must be preserved first. Build and host-test scripts do not flash devices.

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
uv run --locked west flash -d build --cmd-pre-load "reset halt" --cmd-pre-load "nrf5 mass_erase" --verify
uv run --locked python scripts/upload.py scan
uv run --locked python scripts/upload.py upload 'DEVICE_ADDRESS' picture.png --compress
```

Use the scanned address (a peripheral UUID on macOS). Omit the image to upload an
orientation/checkerboard pattern. The helper fits images to 104 × 212 and uses the official
SDK for unauthenticated uploads. Allow at least eight seconds when scanning slow advertising.

For interactive device setup:

```sh
uv run --locked python scripts/set_serial.py
uv run --locked python scripts/set_partial_frames.py
```

These tools back up the configuration, preserve other fields and reconnect to verify writes.
See [BLE identity](docs/ble.md) for serial naming and [T5 display](docs/display.md) for frame
settings, partial-update quality and test strategies.

## Understand and change the firmware

Start with the [architecture](docs/architecture.md) for wiring, memory layout and the command
path. Then read the [protocol](docs/protocol.md), [security](docs/security.md) or
[power and recovery](docs/power.md) topic relevant to your change. The
[documentation index](docs/README.md) is the complete reading map;
[AGENTS.md](AGENTS.md) gives coding agents concise navigation and verification instructions.

## License

Project code is distributed under GNU GPL version 3; see [COPYING](COPYING).
Third-party files retain their notices and licenses; see [NOTICE.md](NOTICE.md).
