"""Apply (or remove before west update) the pinned LT213A buffer patch."""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "scripts/patches/zephyr-4.4.2-compact-buffers.patch"


def patch(root, revert=False):
    command = ["git", "-C", str(root), "apply"]
    applied = subprocess.run(
        [*command, "--reverse", "--check", str(PATCH)], capture_output=True,
    ).returncode == 0
    if revert:
        if applied:
            subprocess.run([*command, "--reverse", str(PATCH)], check=True)
        return
    if not applied:
        subprocess.run([*command, "--check", str(PATCH)], check=True)
        subprocess.run([*command, str(PATCH)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revert", action="store_true")
    args = parser.parse_args()
    patch(ROOT / ".deps/zephyr", args.revert)
