"""Opt-in BLE security regression; backs up and restores the unencrypted config.

uv run --locked python tests/test_security_hardware.py DEVICE BEFORE AFTER
Uses upstream OpenDisplay packet sizes. The ignored build directory holds recovery
config/key files; no keys are printed. Run with SWD available for recovery.
"""
import argparse
import asyncio
import copy
import os
from pathlib import Path
from time import monotonic

from opendisplay import OpenDisplayDevice
from opendisplay.exceptions import AuthenticationFailedError, AuthenticationRequiredError, IntegrityCheckError
from opendisplay.models.config import SecurityConfig
from opendisplay.partial import PartialState
from opendisplay.protocol import build_pipe_write_start_command, build_pipe_write_data_command, serialize_config
from PIL import Image, ImageChops

async def expect_error(kind, operation):
    try:
        await operation
    except kind:
        return
    raise AssertionError(f'Expected {kind.__name__}')

async def check(address, before_path, after_path):
    async with OpenDisplayDevice(mac_address=address, timeout=20) as d:
        original = copy.deepcopy(d.config)
        assert not original.security_config or not original.security_config.encryption_enabled
        d.export_config_json('build/auth-original-config.json')
    key = os.urandom(16)
    key_file = Path('build/auth-test-key.bin')
    key_file.write_bytes(key)
    key_file.chmod(0o600)
    secured = copy.deepcopy(original)
    secured.security_config = SecurityConfig(1, key, 60, 0, 255, bytes(43))
    changed = False
    try:
        async with OpenDisplayDevice(mac_address=address, timeout=20) as d:
            changed = True  # Also attempt recovery if the final ACK is lost.
            await d.write_config(secured)
        print('Temporary security config committed', flush=True)
        async def connect(encryption_key=None):
            async with OpenDisplayDevice(mac_address=address, encryption_key=encryption_key, timeout=20):
                pass
        await expect_error(AuthenticationRequiredError, connect())
        await expect_error(AuthenticationFailedError, connect(bytes(16)))
        print('Missing/wrong key rejected with SDK exceptions', flush=True)
        async with OpenDisplayDevice(mac_address=address, encryption_key=key, timeout=20) as d:
            assert d.config.security_config.encryption_key == key
            good = d._encrypt_frame(b'\0\x44')
            bad = good[:-1] + bytes([good[-1] ^ 1])
            await d._conn.write_command(bad)
            await expect_error(IntegrityCheckError, d._read(5))
            await d._conn.write_command(good)
            assert (await d._read(5))[:2] == b'\0\x44'
            print('Bad CCM tag rejected; valid same nonce still accepted', flush=True)
            # Start a real transfer. Same-nonce DATA is silent and must leave
            # both the session and transfer alive for the following sequence.
            await d._write(build_pipe_write_start_command(False, 1, 1, 157, 2756))
            assert (await d._read(5))[:2] == b'\0\x80'
            first = d._encrypt_frame(build_pipe_write_data_command(0, bytes(16)))
            await d._conn.write_command(first)
            assert (await d._read(5))[:3] == b'\0\x81\0'
            await d._conn.write_command(first)
            await asyncio.sleep(.2)
            await d._write_pipe_frame(build_pipe_write_data_command(1, bytes(16)), response=True)
            assert (await d._read(5))[:3] == b'\0\x81\1'
            print('Duplicate PIPE nonce discarded; next DATA acknowledged', flush=True)
        before = Image.open(before_path).convert('RGB')
        after = Image.open(after_path).convert('RGB')
        assert before.size == after.size == (104, 212)
        async with OpenDisplayDevice(mac_address=address, encryption_key=key, timeout=20) as d:
            await d.upload_image(before, compress=False)
            state = PartialState()
            await d.upload_image(before, compress=True, state=state)
            bounds = ImageChops.difference(before, after).getbbox()
            assert bounds
            rectangle = (bounds[0] // 8 * 8, bounds[1], (bounds[2] + 7) // 8 * 8, bounds[3])
            white = before.copy()
            white.paste('white', rectangle)
            for target in (white, after):
                await d.upload_image(target, state=state)
                assert d._pipe_partial_supported
            Path('build/partial-last-state.bin').write_bytes(state.to_bytes())
            print('Encrypted raw/compressed PIPE and white → target partial uploads passed', flush=True)
        async with OpenDisplayDevice(mac_address=address, encryption_key=key, timeout=20) as d:
            started = monotonic()
            # Bypass SDK's proactive reauthentication to test firmware lifetime.
            for deadline in (50, 61):
                await asyncio.sleep(max(0, deadline - (monotonic() - started)))
                await d._conn.write_command(d._encrypt_frame(b'\0\x44'))
                if deadline == 50:
                    assert (await d._read(5))[:2] == b'\0\x44'
                    print('Authenticated traffic at 50 s accepted', flush=True)
                else:
                    await expect_error(AuthenticationRequiredError, d._read(5))
            print('Session expires at absolute lifetime despite recent valid traffic', flush=True)
    finally:
        if changed:
            async with OpenDisplayDevice(mac_address=address, encryption_key=key, config=original, timeout=20) as d:
                await d.write_config(original)
            async with OpenDisplayDevice(mac_address=address, timeout=20) as d:
                assert serialize_config(d.config) == serialize_config(original)
            key_file.unlink()
            print('Original config restored and verified without key', flush=True)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('address')
    p.add_argument('before', type=Path)
    p.add_argument('after', type=Path)
    a = p.parse_args()
    asyncio.run(check(a.address, a.before, a.after))
