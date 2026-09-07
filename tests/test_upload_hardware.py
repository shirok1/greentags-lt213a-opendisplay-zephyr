"""Real-board regression: upstream OpenDisplay packet sizes, no client overrides.

uv run --locked python tests/test_upload_hardware.py DEVICE IMAGE [--cycles 2]
IMAGE must already be a 104x212 monochrome/native-orientation image.
"""
import argparse
import asyncio
from pathlib import Path

from opendisplay import OpenDisplayDevice
from opendisplay.protocol.commands import CHUNK_SIZE, MAX_START_PAYLOAD
from PIL import Image


async def check(address, path, cycles):
    assert CHUNK_SIZE == 230 and MAX_START_PAYLOAD == 200
    with Image.open(path) as source:
        image = source.convert('1')
    assert image.size == (104, 212)
    data = image.tobytes()
    assert len(data) == 2756
    for cycle in range(cycles):
        for compress in (False, True):
            async with OpenDisplayDevice(mac_address=address, timeout=20) as device:
                version = await device.read_firmware_version()
                assert version['sha'] and version['patch'] == 0
                print(f'Firmware: {version}', flush=True)
                assert device.config.displays[0].supports_pipe_write
                await device.upload_prepared_image((data, None, image), compress=compress)
                assert device._pipe_probed and device._pipe_supported, "Upload fell back from PIPE"
            print(f'Cycle {cycle + 1}: {"compressed" if compress else "raw"} refresh confirmed', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address')
    parser.add_argument('image', type=Path)
    parser.add_argument('--cycles', type=int, default=2)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error('--cycles must be positive')
    asyncio.run(check(args.address, args.image, args.cycles))
