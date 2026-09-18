# Contributing

This port targets one board: nRF51822 QFAB, 128 KiB Flash and 16 KiB RAM.
Keep the Zephyr BLE host/controller, fixed GPIO mapping, and two reserved configuration
pages intact. Do not add SoftDevice or silently advertise unsupported hardware features.

## Development

Follow the setup in [README.md](README.md), then run:

```sh
uv run --locked python scripts/build.py
uv run --locked python scripts/test.py
```

The tests need a native C compiler with AddressSanitizer/UndefinedBehaviorSanitizer.
They verify the protocol, fake-Flash transactions, independent crypto primitives, and
panel GPIO command ordering. They do not establish radio, electrical, or timing correctness.

When changing dependencies, update the Zephyr release tag in `west.yml` (its imported manifest fixes HAL/CMSIS versions) or regenerate
`uv.lock` with uv and include the lockfile change. Do not commit `.deps/`, `.venv/`,
build outputs, personal keys, or device-specific configuration dumps.
`src/config.inc` is intentionally committed; regenerate it with
`uv run --locked python scripts/generate_config.py` when changing default configuration.

Keep changes focused. Explain the behavior change, memory impact, validation performed,
and any remaining hardware uncertainty in your pull request. Preserve third-party
attribution. Do not reformat vendored sources as part of unrelated changes.

## Documentation

Use the [documentation index](docs/README.md) to find the topic affected by a change.
Explain current behavior, design tradeoffs and validation limits in that topic. Keep
experiment conditions with any measurements; avoid appending chronological task reports
or treating old build measurements as current guarantees.

## Hardware reports

Include chip marking/revision, panel model, supply voltage, toolchain versions,
firmware commit, client/OS version, and reproducible steps. For power measurements,
disconnect the debugger and state the advertising/connection/panel state and measurement
setup. Redact BLE identifiers where appropriate; never attach authentication keys or
unredacted persistent-configuration dumps.

## Publishing firmware

Build from the exact source revision being published and retain the ELF/map and memory
report alongside HEX/BIN. Label the panel-sleep variant explicitly. Include corresponding
source and license/third-party notices with distributions, and state which hardware
checks were actually performed. CI artifacts are development builds, not validated releases.
