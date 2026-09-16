"""Opt-in real panel timing test; backs up/restores config and leaves a test pattern.

Requires BLE, SWD, the frame-config firmware, and authentication disabled.
"""
import argparse
import asyncio
import copy
from pathlib import Path
import struct
import sys
import time
import zlib

from bleak import BleakScanner
from opendisplay import OpenDisplayDevice
from opendisplay.protocol import serialize_config
from opendisplay.transport.connection import BLEConnection
from test_alignment_hardware import send, expect, pipe_start, sack, reset

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from set_partial_frames import configure, setting, effective_frames


async def check(address, output):
    peripheral = await BleakScanner.find_device_by_address(address, timeout=30)
    assert peripheral, 'Target not advertising'
    options = dict(mac_address=address, ble_device=peripheral, timeout=20)
    async with OpenDisplayDevice(**options) as device:
        original = copy.deepcopy(device.config)
        assert not original.security_config or not original.security_config.encryption_enabled
    output.mkdir(parents=True, exist_ok=True)
    backup = output / 'original-config.bin'
    if backup.exists():
        assert backup.read_bytes() == serialize_config(original), 'Existing backup differs'
    else:
        with backup.open('xb') as f:
            backup.chmod(0o600)
            f.write(serialize_config(original))
    timings = {}
    try:
        for count in (100, 160):
            await configure(address, count, ble_device=peripheral)
            if count == 160:
                await reset()
            async with OpenDisplayDevice(**options) as device:
                assert effective_frames(setting(device.config)) == count
                print(f'PASS config read-back: {count} frames' + (' after SWD reset' if count == 160 else ''), flush=True)
            async with BLEConnection(address, ble_device=peripheral, timeout=20) as conn:
                # Establish the same full-frame baseline and etag each time.
                raw = bytes(0xaa if (i // 13) % 16 < 8 else 0x55 for i in range(2756))
                compressor = zlib.compressobj(wbits=9)
                zipped = compressor.compress(raw) + compressor.flush()
                await pipe_start(conn, True)
                for index, pos in enumerate(range(0, len(zipped), 200)):
                    await send(conn, 0x81, bytes([index]) + zipped[pos:pos + 200])
                    await sack(conn, index)
                await send(conn, 0x82, b'\0\x12\x34\x56\x78')
                await sack(conn, index)
                await expect(conn, b'\0\x82')
                await expect(conn, b'\0\x73')
                # 32x15 old/new pixels; unchanged contents isolate drive timing.
                await send(conn, 0x80, struct.pack('<BBBBHI IHHHH', 1, 2, 1, 1, 244, 120,
                                                 0x12345678, 40, 11, 32, 15))
                await expect(conn, bytes.fromhex('0080010101f40003'))
                pixels = b''.join(raw[y * 13 + 5:y * 13 + 9] for y in range(11, 26))
                await send(conn, 0x81, b'\0' + pixels + pixels)
                await sack(conn, 0)
                started = time.monotonic()
                await send(conn, 0x82, b'\2\x87\x65\x43\x21')
                await sack(conn, 0)
                await expect(conn, b'\0\x82')
                await expect(conn, b'\0\x73')
                timings[count] = time.monotonic() - started
                print(f'PASS partial refresh: {count} frames, END-to-completion {timings[count]:.3f}s', flush=True)
        assert timings[160] > timings[100] + .5, timings
    finally:
        async with OpenDisplayDevice(**options) as device:
            await device.write_config(original)
        async with OpenDisplayDevice(**options) as device:
            assert serialize_config(device.config) == serialize_config(original)
        print('RESTORED complete original config; effective frames:', effective_frames(setting(original)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address')
    parser.add_argument('--output', type=Path, default=Path('build/partial-frames'))
    args = parser.parse_args()
    asyncio.run(check(args.address, args.output))
