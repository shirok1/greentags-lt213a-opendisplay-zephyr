"""OpenDisplay BLE scanner/uploader for devices with authentication disabled."""
import argparse
import asyncio
import zlib
from pathlib import Path

from bleak import BleakClient, BleakScanner
from PIL import Image, ImageDraw, ImageOps

UUID = "00002446-0000-1000-8000-00805f9b34fb"


def image_bytes(path=None):
    if path:
        with Image.open(path) as source:
            image = ImageOps.pad(source.convert("RGB"), (104, 212), color="white")
    else:
        image = Image.new("RGB", (104, 212), "white")
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 103, 211), outline="black", width=2)
        draw.text((7, 8), "TOP / 104", fill="black")
        draw.text((7, 30), "OpenDisplay", fill="black")
        draw.text((7, 48), "nRF51822", fill="black")
        draw.text((7, 66), "Zephyr BLE", fill="black")
        for y in range(96, 176, 10):
            for x in range(12, 92, 10):
                if (x // 10 + y // 10) % 2:
                    draw.rectangle((x, y, x + 9, y + 9), fill="black")
        draw.text((7, 190), "BOTTOM 212", fill="black")
    result = image.convert("1").tobytes()
    assert len(result) == 2756
    return result


async def scan():
    devices = await BleakScanner.discover(timeout=8, return_adv=True)
    for device, adv in devices.values():
        if 0x2446 in adv.manufacturer_data or UUID in adv.service_uuids:
            print(f"{device.address}  {adv.local_name or device.name}  RSSI={adv.rssi}")


async def upload(address, data, compress=False):
    notifications = asyncio.Queue()
    async with BleakClient(address, timeout=20) as client:
        service = client.services.get_service(UUID)
        characteristic = service.get_characteristic(UUID) if service else None
        if characteristic is None:
            raise RuntimeError("OpenDisplay service/characteristic 0x2446 missing")
        await client.start_notify(characteristic, lambda _, value: notifications.put_nowait(bytes(value)))

        async def expect(opcode, timeout=15):
            value = await asyncio.wait_for(notifications.get(), timeout)
            if len(value) < 2 or value[0] != 0 or value[1] != opcode:
                raise RuntimeError(f"Expected ACK {opcode:02x}, received {value.hex(' ')}")
            return value

        async def command(opcode, payload=b""):
            await client.write_gatt_char(characteristic, bytes([0, opcode]) + payload, response=True)
            return await expect(opcode, timeout=70 if opcode == 0x70 else 15)

        version = await command(0x43)
        print(f"Firmware {version[2]}.{version[3]}")
        header = b""
        if compress:
            header = len(data).to_bytes(4, "little")
            compressor = zlib.compressobj(wbits=9)
            data = compressor.compress(data) + compressor.flush()
        await command(0x70, header)
        # 18 bytes always fit the default ATT MTU=23, including macOS/BlueZ
        # backends that do not expose MTU exchange. Firmware also accepts 230.
        for offset in range(0, len(data), 18):
            await command(0x71, data[offset:offset + 18])
        await command(0x72, b"\0")
        await expect(0x73, timeout=70)
        print("Panel refresh succeeded")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("scan")
    upload_parser = sub.add_parser("upload")
    upload_parser.add_argument("address", help="BLE address or macOS peripheral UUID from scan")
    upload_parser.add_argument("image", nargs="?", type=Path, help="Omit for orientation/checkerboard test")
    upload_parser.add_argument("--compress", action="store_true", help="Send a zlib stream with a 512-byte window")
    args = parser.parse_args()
    if args.action == "scan":
        asyncio.run(scan())
    else:
        asyncio.run(upload(args.address, image_bytes(args.image), args.compress))


if __name__ == "__main__":
    main()
