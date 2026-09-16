"""Set DataExtended.serial_number over BLE, preserving the rest of the config."""
import argparse
import asyncio
import copy
import os
from pathlib import Path
import time

from opendisplay import OpenDisplayDevice
from opendisplay.models.config import DataExtended
from opendisplay.protocol import serialize_config


def serial_bytes(serial):
    encoded = serial.encode('utf-8')
    if len(encoded) > 31 or (serial and not serial.isprintable()):
        raise ValueError('Serial must be printable UTF-8, at most 31 bytes (no NUL/control characters)')
    if serial and not serial.strip():
        raise ValueError('Serial must not consist only of whitespace; use --clear to remove it')
    return encoded.ljust(32, b'\0')


async def set_serial(address, serial, backup=None, key=None):
    encoded = serial_bytes(serial)
    options = dict(mac_address=address, timeout=20, encryption_key=key)
    async with OpenDisplayDevice(**options) as device:
        original = device.config
        old = original.data_extended.serial_number if original.data_extended else bytes(32)
        if old == encoded:
            print('Serial already matches; no configuration write needed')
            return
        updated = copy.deepcopy(original)
        if updated.data_extended is None:
            updated.data_extended = DataExtended()
        updated.data_extended.serial_number = encoded
        expected = serialize_config(updated)
        if len(expected) > 768:
            raise ValueError('Updated configuration exceeds LT213A storage capacity')

        # Config may contain a security key: create the backup private from birth.
        backup = Path(backup) if backup else Path('build/serial-backups') / f'{time.time_ns()}.bin'
        backup.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(serialize_config(original))
        print(f'Configuration backup: {backup}', flush=True)
        await device.write_config(updated)

    # Writing config resets the firmware session. Reconnect, including re-auth.
    async with OpenDisplayDevice(**options) as device:
        if serialize_config(device.config) != expected:
            raise RuntimeError(f'Configuration read-back mismatch; original backup: {backup}')
    print(f'Serial saved and verified: {serial!r}')
    if serial:
        name = 'OD' + serial.encode('utf-8')[:27].decode('utf-8', errors='ignore')
        print(f'Advertising name on serial-aware firmware: {name}')
    else:
        print('Advertising name returns to OD + chip ID')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', help='BLE address, or macOS peripheral UUID from upload.py scan')
    parser.add_argument('serial', nargs='?', help='Printed board serial; case is preserved')
    parser.add_argument('--clear', action='store_true', help='Clear only serial_number, preserving all other fields')
    parser.add_argument('--backup', type=Path, help='New file for the original binary config (never overwritten)')
    parser.add_argument('--key-file', type=Path, help='Optional raw 16-byte key for an authenticated device')
    args = parser.parse_args()
    if args.clear == (args.serial is not None):
        parser.error('Provide either SERIAL or --clear')
    serial = '' if args.clear else args.serial
    try:
        serial_bytes(serial)
    except ValueError as error:
        parser.error(str(error))
    key = args.key_file.read_bytes() if args.key_file else None
    if key is not None and len(key) != 16:
        parser.error('--key-file must contain exactly 16 raw bytes')
    asyncio.run(set_serial(args.address, serial, args.backup, key))


if __name__ == '__main__':
    main()
