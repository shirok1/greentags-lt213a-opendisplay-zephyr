# LT213A OpenDisplay · Zephyr

OpenDisplay-compatible firmware for the **Greentags LT213A**, using an
**nRF51822 QFAB (128 KiB Flash / 16 KiB RAM)** and a **GDEW0213T5 (104 × 212)** e-paper panel.

Built with **Zephyr 3.7.1's BLE host and software controller**. No Nordic SoftDevice.
Python tooling uses **uv**, `pyproject.toml`, and `uv.lock`; firmware builds use west/CMake.
This is an independent port, not an official OpenDisplay firmware release.

[中文使用与实现说明](docs/guide.zh-CN.md) · [Contributing](CONTRIBUTING.md) · [Third-party notices](NOTICE.md)

## Status

**Experimental; full-screen BLE uploads are hardware-tested.** ARM cross-compilation and
host tests pass locally. Official Python package version queries and default PIPE raw/zlib
uploads work on the board. Partial waveforms, authenticated interoperability, worst-case
stack headroom, power consumption, and physical power-loss recovery remain unverified.

- Raw and zlib image streaming, PIPE sequence/SACK handling, configuration persistence,
  and application-layer authentication/encryption are implemented.
- Partial-refresh protocol support is implemented, but its vendor T5 waveform is
  tested through forward/reverse hardware partial updates, with a calibrated waveform; **slight ghosting remains**.
  See [T5 partial test](docs/partial-t5.md). Full-screen fast mode uses the full-refresh waveform.
- No external Flash, LED, buzzer, NFC, power latch, wake button, or battery-sense circuit
  is assumed. Unsupported hardware commands return errors. Firmware updates use **SWD**, not OTA.
- The included BLE uploader uses `py-opendisplay` 7.14.1 (compatible with Python 3.11/3.12)
  for discovery, protocol, compression and refresh confirmation. The helper exposes unauthenticated raw/compressed uploads.

## Wiring

| Signal | nRF51 GPIO | Behavior |
| --- | --- | --- |
| MOSI / SDA | P0.30 | Output, MSB first |
| SCK | P0.00 | Software SPI, mode 0 |
| CS | P0.01 | Active low |
| DC | P0.02 | Low: command; high: data |
| RESET | P0.03 | Active low |
| BUSY | P0.04 | **Low: busy; high: ready**, no internal pull |
| BS | P0.05 | Low, four-wire SPI |

The low-frequency clock uses the internal RC oscillator with calibration.
P0.00 is reserved for SCK, not an external low-frequency crystal.

## Build and test

Install Git, [uv](https://docs.astral.sh/uv/), CMake, Ninja, a native C compiler
(for host tests), and `arm-none-eabi-gcc` or a Zephyr SDK. Run from the repository root:

```sh
uv sync --locked
uv run --locked python scripts/build.py --setup
uv run --locked python scripts/test.py
```

`--setup` fetches the four dependencies pinned in [west.yml](west.yml) into `.deps/`.
Subsequent builds can omit it. Use `--pristine` after changing toolchains.
uv creates and manages the Python environment; manual venv activation is unnecessary.

Build products are in `build/zephyr/`: `zephyr.hex`, `zephyr.bin`, `zephyr.elf`, and
`zephyr.map`. The build checks Flash/RAM bounds and the reset vector automatically.
GitHub Actions builds both default and experimental panel-sleep variants, runs host tests,
and retains build artifacts; a successful CI run is not a hardware certification.

The locally tested Arm GNU 15.3.Rel1 build uses **95,788 B Flash / 16,336 B RAM**.
The application has 126 KiB Flash; the final 2 KiB hold two configuration transaction slots.
Only **48 B of RAM remains outside reserved stacks and buffers**. After the encrypted hardware regression, unused stack prefixes were 252 B (main)
and 380 B (system workqueue); these are observed margins, not worst-case proofs. There is no full-frame MCU buffer or dynamic heap.

## Flash and upload

Connect J-Link using SWDIO, SWCLK, GND, and target voltage reference.
The following command **erases the chip, including saved configuration**; back up any
firmware/configuration you need first. No device is flashed by build or test scripts.

```sh
export ZEPHYR_BASE="$PWD/.deps/zephyr"
uv run --locked west flash -d build -r jlink --erase --reset
uv run --locked python scripts/upload.py scan
uv run --locked python scripts/upload.py upload 'DEVICE_ADDRESS'
uv run --locked python scripts/upload.py upload 'DEVICE_ADDRESS' picture.png --compress
```

Use the address reported by scanning (a peripheral UUID on macOS). Omitting the image
uploads an orientation/checkerboard pattern. Images are fitted to 104 × 212 and converted
to monochrome. Scan for at least eight seconds when using slow advertising.

## Protocol and power behavior

The BLE service and characteristic both use `00002446-0000-1000-8000-00805f9b34fb`.
The protocol reference is OpenDisplay/Firmware commit
[`7c9413e`](https://github.com/OpenDisplay/Firmware/tree/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb).

| Capability | Bound / behavior |
| --- | --- |
| Full image | 2,756 bytes, row-major, MSB first; 0 black, 1 white |
| Direct-write data | Up to 230 data bytes per command; upstream uploader defaults (230-byte DATA / 200-byte START) |
| Compression | Streaming zlib, 512-byte window, Adler32 validation |
| PIPE | Negotiated window and ACK interval both 1; duplicate suppression and SACK |
| Configuration | Up to 768 bytes, CRC validation, two-slot Flash commit |
| Authentication | CMAC mutual authentication, CCM frames, replay checks and expiry; disabled by default |
| Advertising | 160 ms for the initial 10 seconds, then 1 second; transition deferred while connected |
| Idle connection | Disconnect after 120 seconds without accepted business commands |
| Incomplete transfer | Abort after 30 seconds of application inactivity |
| Telemetry | One-minute advertising heartbeat; temperature sampled at most every five minutes |

The panel is powered off on startup and after refresh. Experimental `07 A5` panel
sleep can be built with `--epd-deep-sleep`; it is **off by default** pending T5 validation.
Building again without the flag restores the default. CPU idle uses Zephyr event waits;
System OFF is not used because this board lacks a practical wake source.

## Repository layout

- `src/` — protocol, security, persistent configuration, BLE application, panel driver.
- `boards/greentags/lt213a/` — board definition, pins, memory partitions, SWD runners.
- `scripts/` — uv-managed build, upload, memory checks, and test entry points.
- `tests/` — host tests, fake Flash and virtual GPIO/clock support.
- `third_party/uzlib/` — attributed streaming decompressor sources.
- `docs/` — detailed Chinese implementation and validation guide.

## License

Project code is distributed under the GNU GPL version 3; see [COPYING](COPYING).
Third-party files retain their own notices and licenses; see [NOTICE.md](NOTICE.md).
