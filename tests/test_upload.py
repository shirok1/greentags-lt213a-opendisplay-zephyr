"""Check that the package receives exact monochrome pixels and compression intent."""
import asyncio
import sys
from pathlib import Path
from unittest.mock import AsyncMock, patch
import opendisplay.device as opendisplay_device
from opendisplay.protocol.responses import parse_firmware_version

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import upload


async def check():
    assert opendisplay_device.CHUNK_SIZE == 230
    assert opendisplay_device.MAX_START_PAYLOAD == 200
    version = parse_firmware_version(Path('build/test-version.bin').read_bytes())
    assert version['major'] == 0 and version['minor'] == 2 and version['patch'] == 0
    assert version['sha']
    pixels = bytes(i % 256 for i in range(2756))
    device = AsyncMock()
    device.__aenter__.return_value = device
    with patch.object(upload, 'OpenDisplayDevice', return_value=device) as constructor:
        for compress in (False, True):
            await upload.upload('device-id', pixels, compress)
            constructor.assert_called_with(mac_address='device-id', timeout=20)
            args, kwargs = device.upload_prepared_image.call_args
            raw, zipped, image = args[0]
            assert raw == pixels and zipped is None
            assert image.size == (104, 212) and image.tobytes() == pixels
            assert kwargs == {'compress': compress}
        device.upload_prepared_image.side_effect = RuntimeError('refresh failed')
        try:
            await upload.upload('device-id', pixels)
        except RuntimeError as error:
            assert str(error) == 'refresh failed'
        else:
            raise AssertionError('Upload failure was swallowed')
    print('OpenDisplay package delegation passed')


if __name__ == '__main__':
    asyncio.run(check())
