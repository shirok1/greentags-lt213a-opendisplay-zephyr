"""Check preservation, private backups and read-back verification in the CLI."""
import asyncio
import copy
from pathlib import Path
import stat
import sys
import tempfile
from unittest.mock import AsyncMock, patch

from opendisplay.models.config import DataExtended
from opendisplay.protocol import serialize_config
from opendisplay.protocol.config_parser import parse_config_response

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import set_serial


async def check():
    original = parse_config_response(Path('build/secure-config.bin').read_bytes())
    original.data_extended = DataExtended.from_strings(
        serial_number='old', friendly_name='Shelf 7', custom_string_1='18111')
    original_bytes = serialize_config(original)
    for serial in ('2402859c', ''):
        expected = copy.deepcopy(original)
        expected.data_extended.serial_number = set_serial.serial_bytes(serial)
        source, readback = AsyncMock(), AsyncMock()
        source.__aenter__.return_value = source
        source.config = original
        readback.__aenter__.return_value = readback
        readback.config = expected
        with tempfile.TemporaryDirectory() as directory:
            backup = Path(directory) / 'config.bin'
            with patch.object(set_serial, 'OpenDisplayDevice', side_effect=[source, readback]):
                await set_serial.set_serial('address', serial, backup, bytes(range(16)))
            assert backup.read_bytes() == original_bytes
            assert stat.S_IMODE(backup.stat().st_mode) == 0o600
            written = source.write_config.call_args.args[0]
            assert serialize_config(written) == serialize_config(expected)
            assert serialize_config(original) == original_bytes
            assert written.data_extended.friendly_name_text == 'Shelf 7'
            assert written.data_extended.custom_string_1_text == '18111'
            assert written.security_config == original.security_config
            # Never overwrite the recovery file or write after backup failure.
            source.write_config.reset_mock()
            with patch.object(set_serial, 'OpenDisplayDevice', return_value=source):
                try:
                    await set_serial.set_serial('address', serial, backup)
                except FileExistsError:
                    pass
                else:
                    raise AssertionError('Existing backup was overwritten')
            source.write_config.assert_not_called()

    # Real parser/serializer without an extended packet: add only that packet.
    plain = parse_config_response(Path('build/test-config.bin').read_bytes())
    source, readback = AsyncMock(), AsyncMock()
    source.__aenter__.return_value = source
    source.config = plain
    readback.__aenter__.return_value = readback
    readback.config = plain  # Device ignored the write: must not report success.
    with tempfile.TemporaryDirectory() as directory:
        with patch.object(set_serial, 'OpenDisplayDevice', side_effect=[source, readback]):
            try:
                await set_serial.set_serial('address', '2402859c', Path(directory) / 'before.bin')
            except RuntimeError as error:
                assert 'read-back mismatch' in str(error)
            else:
                raise AssertionError('Failed write was reported as successful')
    for bad in ('x' * 32, '中' * 11, 'ab\0cd', 'ab\ncd', '   '):
        try:
            set_serial.serial_bytes(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Invalid serial accepted: {bad!r}')
    assert len(set_serial.serial_bytes('中' * 10)) == 32
    print('Serial CLI passed: config/security preserved, private backup, bounds and verification failure')



if __name__ == '__main__':
    asyncio.run(check())
