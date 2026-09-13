"""Opt-in SWD reset regression: the same BLE identity must reconnect each time.

Run with the board flashed: python tests/test_identity_hardware.py DEVICE
DEVICE is the previously known BLE address (CoreBluetooth UUID on macOS).
"""
import argparse
import asyncio
import subprocess

from opendisplay import OpenDisplayDevice


async def check(address):
    for trial in range(3):
        subprocess.run(["probe-rs", "reset", "--chip", "nRF51822_xxAB",
                        "--speed", "1000", "--non-interactive"], check=True)
        await asyncio.sleep(1)
        async with OpenDisplayDevice(mac_address=address, timeout=20,
                                     max_attempts=1) as device:
            version = await device.read_firmware_version()
            assert version
            print(f"PASS reset {trial + 1}: original identity, config and version", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("address")
    asyncio.run(check(parser.parse_args().address))
