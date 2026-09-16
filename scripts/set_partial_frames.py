"""Configure LT213A partial-refresh drive frames in OpenDisplay metadata."""
import argparse
import asyncio
import copy
import os
from pathlib import Path
import time

from opendisplay import OpenDisplayDevice
from opendisplay.models.config import DataExtended
from opendisplay.protocol import serialize_config
from set_serial import scan_serials, show_devices

KEY = 'lt213a.partial_frames='
DEFAULT = 100


def effective_frames(value):
    if not value.startswith(KEY):
        return DEFAULT
    number = value[len(KEY):]
    if not number or any(c < '0' or c > '9' for c in number):
        return DEFAULT
    result = int(number)
    return result if 1 <= result <= 255 else DEFAULT


def setting(config):
    return config.data_extended.custom_string_3_text if config.data_extended else ''


def changed_config(original, frames, reset=False):
    if not reset and (type(frames) is not int or not 1 <= frames <= 255):
        raise ValueError('帧数必须是 1–255 的整数')
    old = setting(original)
    if old and not old.startswith(KEY):
        raise ValueError('custom_string_3 已有其他用途，不会覆盖；请先迁移该字段的内容')
    updated = copy.deepcopy(original)
    if updated.data_extended is None:
        if reset:
            return updated
        updated.data_extended = DataExtended()
    value = '' if reset else f'{KEY}{frames}'
    updated.data_extended.custom_string_3 = value.encode('ascii').ljust(32, b'\0')
    return updated


async def configure(address, frames=None, *, reset=False, key=None, ble_device=None, prompt=False):
    options = dict(mac_address=address, timeout=20, encryption_key=key)
    if ble_device is not None:
        options['ble_device'] = ble_device
    async with OpenDisplayDevice(**options) as device:
        original = copy.deepcopy(device.config)
    current = setting(original)
    print(f'配置帧数：{effective_frames(current)}；custom_string_3={current!r}')
    if prompt:
        value = input('输入帧数 1–255，d 恢复默认100，留空退出：').strip()
        if not value:
            return
        reset = value.lower() == 'd'
        frames = None if reset else int(value)
    if frames is None and not reset:
        return
    updated = changed_config(original, frames, reset)
    expected = serialize_config(updated)
    if expected == serialize_config(original):
        print('配置已匹配，无需写入。')
        return
    if len(expected) > 768:
        raise ValueError('更新后的配置超过 LT213A 的768字节限制')
    backup = Path('build/partial-frame-backups') / f'{time.time_ns()}.bin'
    backup.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(serialize_config(original))
    print(f'原配置已备份：{backup}', flush=True)
    async with OpenDisplayDevice(**options) as device:
        # Interactive entry can take time: never overwrite another client's edit.
        if serialize_config(device.config) != serialize_config(original):
            raise RuntimeError('设备配置已被其他操作修改，请重新读取后再设置')
        await device.write_config(updated)
    async with OpenDisplayDevice(**options) as device:
        if serialize_config(device.config) != expected:
            raise RuntimeError(f'完整配置回读不一致；原配置备份：{backup}')
    print(f'已保存并回读验证：{effective_frames(setting(updated))} 帧。')
    print('包含此配置扩展的固件将在下一次局刷生效，无需重启。')


async def interactive(key):
    while True:
        rows = show_devices(await scan_serials(key=key))
        choice = input('选择设备编号，r 重扫，q 退出：').strip().lower()
        if choice in ('q', ''):
            return
        if choice == 'r':
            continue
        if not choice.isdecimal() or not 1 <= int(choice) <= len(rows):
            print('编号不在本轮列表中。')
            continue
        row = rows[int(choice) - 1]
        if row.serial is None:
            print('配置读取失败，请先解决连接/密钥问题。')
            continue
        await configure(row.device.address, key=key, ble_device=row.device, prompt=True)
        return


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address', nargs='?', help='省略则扫描并按编号选择设备')
    parser.add_argument('frames', nargs='?', type=int, help='1–255；只传地址则读取当前设置')
    parser.add_argument('--reset', action='store_true', help='清除本工具的设置，恢复默认100帧')
    parser.add_argument('--key-file', type=Path, help='已开启认证时使用的原始16字节密钥文件')
    args = parser.parse_args()
    if args.reset and args.frames is not None:
        parser.error('--reset 与 frames 不能同时使用')
    if args.frames is not None and not 1 <= args.frames <= 255:
        parser.error('frames 必须在1–255之间')
    if args.reset and args.address is None:
        parser.error('--reset 需要设备地址；交互模式中可输入 d 恢复默认')
    key = args.key_file.read_bytes() if args.key_file else None
    if key is not None and len(key) != 16:
        parser.error('--key-file 必须包含16个原始字节')
    try:
        if args.address:
            asyncio.run(configure(args.address, args.frames, reset=args.reset, key=key))
        else:
            asyncio.run(interactive(key))
    except (EOFError, KeyboardInterrupt):
        print('\n已退出。')
    except (ValueError, RuntimeError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
