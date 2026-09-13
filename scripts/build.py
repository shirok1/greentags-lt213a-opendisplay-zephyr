"""Run with uv run --locked python scripts/build.py [--setup] [--pristine]."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".deps"


def run(*args, cwd=ROOT, env=None):
    subprocess.run(args, cwd=cwd, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--setup", action="store_true", help="Fetch pinned Zephyr and HAL sources")
    parser.add_argument("--pristine", action="store_true")
    parser.add_argument("--epd-deep-sleep", action="store_true", help="Explicitly select default panel deep sleep")
    parser.add_argument("--no-epd-deep-sleep", action="store_true", help="Use power-off only for comparison")
    args = parser.parse_args()
    if args.epd_deep_sleep and args.no_epd_deep_sleep:
        parser.error("Choose only one panel power mode")
    manifest = DEPS / "manifest"
    if args.setup:
        manifest.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "west.yml", manifest / "west.yml")
        if not (DEPS / ".west").exists():
            run("west", "init", "-l", str(manifest), cwd=DEPS)
        run("west", "config", "manifest.path", "manifest", cwd=DEPS)
        run("west", "update", "--narrow", cwd=DEPS)
    if not all((DEPS / path).is_dir() for path in (
        "zephyr", "modules/hal/nordic/nrfx", "modules/hal/cmsis",
        "modules/crypto/tinycrypt/lib/source",
    )):
        parser.error("Run with --setup first")
    env = os.environ.copy()
    env["ZEPHYR_BASE"] = str(DEPS / "zephyr")
    if "ZEPHYR_TOOLCHAIN_VARIANT" not in env:
        compiler = shutil.which("arm-none-eabi-gcc")
        if compiler:
            env["ZEPHYR_TOOLCHAIN_VARIANT"] = "gnuarmemb"
            env.setdefault("GNUARMEMB_TOOLCHAIN_PATH", str(Path(compiler).resolve().parent.parent))
        else:
            env["ZEPHYR_TOOLCHAIN_VARIANT"] = "zephyr"
    run("west", "build", "-b", "lt213a/nrf51822", "-d", str(ROOT / "build"),
        *( ["-p", "always"] if args.pristine else [] ), str(ROOT), "--",
        "-DEXTRA_CONF_FILE=" + str(ROOT / ("epd-power-off.conf" if args.no_epd_deep_sleep else "epd-deep-sleep.conf")), env=env)
    run(sys.executable, str(ROOT / "scripts/check_memory.py"), str(ROOT / "build/zephyr/zephyr.elf"))


if __name__ == "__main__":
    main()
