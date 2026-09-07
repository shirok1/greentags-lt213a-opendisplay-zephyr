"""OpenDisplay BLE scanner/uploader for devices with authentication disabled."""
import argparse
import asyncio
from pathlib import Path

from opendisplay import OpenDisplayDevice, discover_devices
from PIL import Image, ImageDraw, ImageOps


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
    for name, address in (await discover_devices(timeout=8)).items():
        print(f"{address}  {name}")


async def upload(address, data, compress=False):
    async with OpenDisplayDevice(mac_address=address, timeout=20) as device:
        image = Image.frombytes("1", (104, 212), data)
        await device.upload_prepared_image((data, None, image), compress=compress)
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
