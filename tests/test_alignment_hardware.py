"""Opt-in PIPE/config regression on an LT213A connected over BLE and SWD.

Back up Flash separately before running. Saves and restores the exact config
container; requires authentication disabled. Uploads a generated test pattern.
--baseline records the two known pre-fix PIPE differences without failing.
"""
import argparse
import asyncio
import binascii
from pathlib import Path
import struct
import time
import zlib

from opendisplay.exceptions import BLETimeoutError
from opendisplay.transport.connection import BLEConnection


def container(body):
    data = bytearray(body)
    data[:2] = b'\0\0'
    data += binascii.crc_hqx(data, 0xffff).to_bytes(2, 'little')
    data[:2] = len(data).to_bytes(2, 'little')
    return bytes(data)


async def send(conn, opcode, payload=b''):
    await conn.write_command(bytes([0, opcode]) + payload, drain_stale=False)


async def expect(conn, value, timeout=40):
    actual = await conn.read_response(timeout)
    assert actual == value, (actual.hex(), value.hex())


async def ack(conn, opcode, ok=True):
    actual = await conn.read_response(40)
    assert actual[:2] == bytes([0 if ok else 255, opcode]), actual.hex()
    assert len(actual) in (2, 4), actual.hex()
    return actual


async def quiet(conn):
    try:
        return await conn.read_response(.5)
    except BLETimeoutError:
        return None


async def read_config(conn):
    await send(conn, 0x40)
    result = bytearray()
    chunk = 0
    total = None
    while total is None or len(result) < total:
        frame = await conn.read_response(10)
        assert frame[:4] == b'\0\x40' + chunk.to_bytes(2, 'little'), frame.hex()
        if chunk == 0:
            total = int.from_bytes(frame[4:6], 'little')
        result += frame[6 if chunk == 0 else 4:]
        chunk += 1
    assert len(result) == total
    assert container(result[:-2]) == result, 'read-back length/CRC mismatch'
    return bytes(result)


async def write_config(conn, data, ok=True):
    if len(data) <= 200:
        await send(conn, 0x41, data)
        return await ack(conn, 0x41, ok)
    await send(conn, 0x41, len(data).to_bytes(2, 'little') + data[:200])
    await ack(conn, 0x41)
    for pos in range(200, len(data), 200):
        await send(conn, 0x42, data[pos:pos + 200])
        result = await ack(conn, 0x42, ok if pos + 200 >= len(data) else True)
    return result


async def reset():
    process = await asyncio.create_subprocess_exec('probe-rs', 'reset', '--chip',
        'nRF51822_xxAB', '--speed', '1000', '--non-interactive')
    assert await process.wait() == 0
    await asyncio.sleep(1)


async def pipe_start(conn, compressed=False):
    await send(conn, 0x80, struct.pack('<BBBBHI', 1, int(compressed), 32, 32, 244, 2756))
    await expect(conn, bytes.fromhex('0080010101f40001'))


async def sack(conn, index):
    mask = (1 << min(index, 32)) - 1
    await expect(conn, b'\0\x81' + bytes([index & 255]) + mask.to_bytes(4, 'little'))


async def pipe_checks(conn, baseline):
    # Seven-byte data frames force the 8-bit sequence through 255 -> 0.
    raw = bytes(0xaa if (i // 13) % 16 < 8 else 0x55 for i in range(2756))
    await pipe_start(conn)
    started = time.monotonic()
    for index, pos in enumerate(range(0, len(raw), 7)):
        frame = bytes([index & 255]) + raw[pos:pos + 7]
        await send(conn, 0x81, frame)
        await sack(conn, index)
        if index in (0, 31, 254, 255, 256):
            # Act as a sender whose ACK was lost: retransmit identical DATA.
            await send(conn, 0x81, frame)
            await sack(conn, index)
    await expect(conn, b'\0\x82')
    await expect(conn, b'\0\x73')
    print(f'PASS raw: W=N=1, 394 frames, seq wrap, five retries, SACK -> END -> refresh ({time.monotonic()-started:.2f}s)', flush=True)
    await send(conn, 0x81, frame)
    late = await quiet(conn)
    print('Post-auto-END duplicate DATA:', late.hex() if late else 'silent', flush=True)
    assert baseline or late is None
    await send(conn, 0x82, b'\0')
    await expect(conn, b'\xff\x82')
    assert await quiet(conn) is None, 'late END triggered another refresh'

    # A frame outside negotiated W=1 is fatal; later DATA must be silent.
    await pipe_start(conn)
    await send(conn, 0x81, b'\1\xff')
    error = await conn.read_response(40)
    assert error[:3] == b'\xff\x81\4' and len(error) == 8, error.hex()
    await send(conn, 0x81, b'\0\xff')
    assert await quiet(conn) is None
    print('PASS out-of-window fatal NACK, subsequent DATA discarded', flush=True)

    # END before all raw data: upstream flushes a SACK even on failure.
    await pipe_start(conn)
    await send(conn, 0x81, b'\0\xff')
    await sack(conn, 0)
    await send(conn, 0x82, b'\0')
    first = await conn.read_response(40)
    if first[:2] == b'\0\x81':
        await expect(conn, b'\xff\x82')
    else:
        assert baseline and first == b'\xff\x82', first.hex()
    print('Incomplete END first response:', first.hex(), flush=True)
    assert await quiet(conn) is None

    compressor = zlib.compressobj(wbits=9)
    zipped = compressor.compress(raw) + compressor.flush()
    await pipe_start(conn, True)
    for index, pos in enumerate(range(0, len(zipped), 200)):
        await send(conn, 0x81, bytes([index]) + zipped[pos:pos + 200])
        await sack(conn, index)
    assert await quiet(conn) is None, 'compressed transfer auto-ended'
    await send(conn, 0x82, b'\0\x12\x34\x56\x78')
    await sack(conn, index)
    await expect(conn, b'\0\x82')
    await expect(conn, b'\0\x73')
    # Raw PIPE partial must also wait for END, despite exact byte count.
    payload = struct.pack('<BBBBHI IHHHH', 1, 2, 32, 32, 244, 16,
                          0x12345678, 0, 0, 8, 8)
    await send(conn, 0x80, payload)
    await expect(conn, bytes.fromhex('0080010101f40003'))
    pixels = bytes(raw[row * 13] for row in range(8))
    await send(conn, 0x81, b'\0' + pixels + pixels)
    await sack(conn, 0)
    assert await quiet(conn) is None, 'partial transfer auto-ended'
    await send(conn, 0x82, b'\2\x87\x65\x43\x21')
    await sack(conn, 0)
    await expect(conn, b'\0\x82')
    await expect(conn, b'\0\x73')
    print('PASS compressed/partial require explicit END; etag accepted', flush=True)


async def fast_checks(address):
    """Explicit compressed END selects FULL/FAST; raw PIPE auto-END cannot."""
    timings = {0: [], 1: []}
    async with BLEConnection(address, timeout=20, max_attempts=2) as conn:
        raw = bytes(0xaa if (i // 13) % 16 < 8 else 0x55 for i in range(2756))
        compressor = zlib.compressobj(wbits=9)
        zipped = compressor.compress(raw) + compressor.flush()
        for trial in range(3):
            for mode in (0, 1):
                await pipe_start(conn, True)
                for index, pos in enumerate(range(0, len(zipped), 200)):
                    await send(conn, 0x81, bytes([index]) + zipped[pos:pos + 200])
                    await sack(conn, index)
                started = time.monotonic()
                await send(conn, 0x82, bytes([mode]))
                await sack(conn, index)
                await expect(conn, b'\0\x82')
                await expect(conn, b'\0\x73')
                elapsed = time.monotonic() - started
                timings[mode].append(elapsed)
                print(f'PASS explicit END mode={mode}, trial={trial + 1}: {elapsed:.3f}s', flush=True)
    for mode, samples in timings.items():
        print(f'Mode {mode}: median END-to-refresh notification = {sorted(samples)[1]:.3f}s', flush=True)


async def check(args):
    def connect():
        return BLEConnection(args.address, timeout=20, max_attempts=2)

    async with connect() as conn:
        await send(conn, 0x43)
        print('Firmware version wire:', (await conn.read_response()).hex(), flush=True)
        original = await read_config(conn)
        # This test intentionally does not provision or change authentication.
        assert len(original) == 133, 'requires the default, unencrypted fixed-board config'
        backup = args.output / 'original-config.bin'
        args.output.mkdir(parents=True, exist_ok=True)
        if backup.exists():
            assert backup.read_bytes() == original, 'existing backup differs; do not overwrite'
        else:
            backup.write_bytes(original)
            backup.chmod(0o600)
        await pipe_checks(conn, args.baseline)

    # Manufacturer board revision is inert metadata, safe to vary for persistence.
    small = bytearray(original[:-2])
    small[32] = 0x51
    small = container(small)
    extended = container(small[:-2] + b'\0\x2c' + b'alignment-test\0'.ljust(288, b'\0'))
    changed = False
    try:
        async with connect() as conn:
            changed = True
            result = await write_config(conn, small)
            assert await read_config(conn) == small
            print('PASS single-packet config, response:', result.hex(), flush=True)
            await write_config(conn, extended)
            assert await read_config(conn) == extended
            corrupt = extended[:-1] + bytes([extended[-1] ^ 1])
            await write_config(conn, corrupt, False)
            assert await read_config(conn) == extended
            print('PASS 423-byte multi-packet config; bad CRC rejected and old config retained', flush=True)
        await reset()
        async with connect() as conn:
            assert await read_config(conn) == extended
            # Leave an incomplete replacement, then reset with staging in progress.
            await send(conn, 0x41, len(extended).to_bytes(2, 'little') + original[:100].ljust(200, b'\0'))
            await ack(conn, 0x41)
            await reset()
        async with connect() as conn:
            assert await read_config(conn) == extended
            print('PASS SWD reset after commit and during incomplete write retains committed config', flush=True)
            await send(conn, 0x41, len(extended).to_bytes(2, 'little') + extended[:200])
            await ack(conn, 0x41)
        async with connect() as conn:
            await send(conn, 0x42, extended[200:400])
            await ack(conn, 0x42, False)
            assert await read_config(conn) == extended
            # A continuation that exceeds declared length must not commit.
            await send(conn, 0x41, len(extended).to_bytes(2, 'little') + extended[:200])
            await ack(conn, 0x41)
            await send(conn, 0x42, extended[200:400])
            await ack(conn, 0x42)
            await send(conn, 0x42, extended[400:] + b'\0')
            await ack(conn, 0x42, False)
            assert await read_config(conn) == extended
            print('PASS disconnect cancels staging; overrun rejected without losing old config', flush=True)
            await send(conn, 0x45)
            result = await ack(conn, 0x45)
            assert await read_config(conn) == original
            print('PASS clear restores fixed-board defaults, response:', result.hex(), flush=True)
        await reset()
        async with connect() as conn:
            assert await read_config(conn) == original
            print('PASS defaults survive SWD reset', flush=True)
    finally:
        if changed:
            async with connect() as conn:
                await write_config(conn, original)
                assert await read_config(conn) == original
            print('RESTORED exact original configuration', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address')
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--fast-only', action='store_true', help='Compare explicit FULL/FAST END; does not modify config')
    parser.add_argument('--output', type=Path, default=Path('build/alignment-2026-09-16'))
    args = parser.parse_args()
    asyncio.run(fast_checks(args.address) if args.fast_only else check(args))
