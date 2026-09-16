"""Scan OpenDisplay serial status and enter serial numbers without copying addresses."""
import argparse
import asyncio
import copy
from dataclasses import dataclass
import os
from pathlib import Path
import time

from bleak import BleakScanner
from opendisplay import OpenDisplayDevice
from opendisplay.exceptions import AuthenticationRequiredError, AuthenticationFailedError
from opendisplay.models.config import DataExtended
from opendisplay.protocol import serialize_config


def serial_bytes(serial):
    encoded = serial.encode('utf-8')
    if len(encoded) > 31 or (serial and not serial.isprintable()):
        raise ValueError('Serial must be printable UTF-8, at most 31 bytes (no NUL/control characters)')
    if serial and not serial.strip():
        raise ValueError('Serial must not consist only of whitespace; use --clear to remove it')
    return encoded.ljust(32, b'\0')


async def set_serial(address, serial, backup=None, key=None, *, ble_device=None):
    encoded = serial_bytes(serial)
    options = dict(mac_address=address, timeout=20, encryption_key=key)
    if ble_device is not None:
        options['ble_device'] = ble_device
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


@dataclass
class SerialDevice:
    device: object
    name: str
    rssi: int
    serial: str | None = None  # None = unknown, '' = confirmed unset.
    error: str = ''


async def scan_serials(seconds=12, timeout=20, key=None):
    print(f'扫描广播 {seconds:g} 秒，然后逐台连接读取 serial_number…', flush=True)
    advertisements = await BleakScanner.discover(timeout=seconds, return_adv=True)
    rows = []
    for device, adv in advertisements.values():
        # Names may be absent or cached; identify OD from the advertisement.
        if 0x2446 not in adv.manufacturer_data and '00002446-0000-1000-8000-00805f9b34fb' not in adv.service_uuids:
            continue
        name = adv.local_name or (f'{device.name}（缓存名）' if device.name else '(未收到名称)')
        rows.append(SerialDevice(device, name, adv.rssi))
    rows.sort(key=lambda row: (-row.rssi, row.device.address))
    for index, row in enumerate(rows, 1):
        print(f'读取 {index}/{len(rows)}：{row.name!r}  {row.device.address}', flush=True)
        try:
            async with asyncio.timeout(timeout):
                async with OpenDisplayDevice(mac_address=row.device.address, ble_device=row.device,
                        timeout=timeout, max_attempts=1, encryption_key=key) as device:
                    extended = device.config.data_extended
                    row.serial = extended.serial_number_text if extended else ''
        except AuthenticationRequiredError:
            row.error = '需要密钥（--key-file）'
        except AuthenticationFailedError:
            row.error = '密钥不匹配'
        except TimeoutError:
            row.error = '连接/读取超时'
        except Exception as error:
            # A failed connection is NOT evidence of an unset serial.
            row.error = f'{type(error).__name__}: {str(error)[:160]}'
    return rows


def show_devices(rows, only_unset=False):
    unset = sum(row.serial == '' for row in rows)
    unknown = sum(row.serial is None for row in rows)
    print(f'\n本轮发现 {len(rows)} 台：未设置 {unset}，已设置 {len(rows) - unset - unknown}，未知 {unknown}')
    visible = [row for row in rows if not only_unset or row.serial in ('', None)]
    if not visible:
        print('没有符合条件的设备。可增加 --seconds 或重新扫描。')
        return visible
    print('编号  状态     serial_number    RSSI    名称    地址')
    for index, row in enumerate(visible, 1):
        status = '未知' if row.serial is None else '未设置' if not row.serial else '已设置'
        serial = repr(row.serial) if row.serial else '—'
        print(f'[{index}]  {status}  {serial}  {row.rssi} dBm  {row.name!r}  {row.device.address}')
        if row.error:
            print(f'     原因：{row.error!r}')
    print('状态来自连接后的配置读取，不按广播名称猜测；未知设备不算未设置。')
    return visible


async def interactive(seconds=12, timeout=20, key=None, backup=None, only_unset=False):
    while True:
        try:
            rows = await scan_serials(seconds, timeout, key)
        except Exception as error:
            print(f'扫描失败：{str(error)!r}')
            if input('输入 r 重试，其他输入退出：').strip().lower() == 'r':
                continue
            return
        visible = show_devices(rows, only_unset)
        while True:
            choice = input('\n输入编号录入 serial，r 重新扫描，q 退出：').strip().lower()
            if choice == 'q':
                return
            if choice == 'r':
                break
            if not choice.isdecimal() or not 1 <= int(choice) <= len(visible):
                print('请输入本轮列表里的编号。')
                continue
            row = visible[int(choice) - 1]
            if row.serial is None:
                print('该设备配置尚未读到，请先解决连接/密钥问题并重新扫描。')
                continue
            print(f'选中 {row.name!r} / {row.device.address}，当前 serial：{row.serial!r}')
            serial = input('输入新 serial（留空取消）：')
            if not serial:
                continue
            try:
                serial_bytes(serial)
                await set_serial(row.device.address, serial, backup, key, ble_device=row.device)
            except Exception as error:
                print(f'录入失败：{str(error)!r}；请重新扫描确认当前状态。')
            # Rebuild the list; numbers never refer to a previous scan.
            break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', nargs='?', help='省略进入交互录入；scan 只查看；也可直接填 BLE 地址/UUID')
    parser.add_argument('serial', nargs='?', help='Printed board serial; case is preserved')
    parser.add_argument('--clear', action='store_true', help='Clear only serial_number, preserving all other fields')
    parser.add_argument('--backup', type=Path, help='New file for the original binary config (never overwritten)')
    parser.add_argument('--key-file', type=Path, help='Optional raw 16-byte key for an authenticated device')
    parser.add_argument('--seconds', type=float, default=12, help='广播扫描时长，默认12秒')
    parser.add_argument('--timeout', type=float, default=20, help='每台设备读取配置的最长秒数，默认20')
    parser.add_argument('--unset', action='store_true', help='只列出未设置与状态未知的设备')
    args = parser.parse_args()
    if not 0 < args.seconds <= 300 or not 0 < args.timeout <= 300:
        parser.error('--seconds and --timeout must be between 0 and 300 seconds')
    key = args.key_file.read_bytes() if args.key_file else None
    if key is not None and len(key) != 16:
        parser.error('--key-file must contain exactly 16 raw bytes')
    if args.address in (None, 'scan'):
        if args.serial is not None or args.clear:
            parser.error('SERIAL/--clear requires a device address; omit both for scan or interactive mode')
        if args.address == 'scan':
            show_devices(asyncio.run(scan_serials(args.seconds, args.timeout, key)), args.unset)
        else:
            try:
                asyncio.run(interactive(args.seconds, args.timeout, key, args.backup, args.unset))
            except (EOFError, KeyboardInterrupt):
                print('\n已退出。')
        return
    if args.clear == (args.serial is not None):
        parser.error('Provide either SERIAL or --clear')
    serial = '' if args.clear else args.serial
    try:
        serial_bytes(serial)
    except ValueError as error:
        parser.error(str(error))
    asyncio.run(set_serial(args.address, serial, args.backup, key))


if __name__ == '__main__':
    main()
