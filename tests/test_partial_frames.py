"""Configuration tool boundaries, metadata preservation and verified writes."""
import asyncio
import contextlib
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
import set_partial_frames as tool


async def check():
    original = parse_config_response(Path('build/secure-config.bin').read_bytes())
    original.data_extended = DataExtended.from_strings(serial_number='2402859c', custom_string_1='18111')
    before = serialize_config(original)
    for count in (1, 100, 160, 255):
        updated = tool.changed_config(original, count)
        assert tool.effective_frames(tool.setting(updated)) == count
        assert updated.security_config == original.security_config
        assert updated.data_extended.serial_number_text == '2402859c'
        assert updated.data_extended.custom_string_1_text == '18111'
        reset = tool.changed_config(updated, None, reset=True)
        assert serialize_config(reset) == before
    assert serialize_config(original) == before
    for count in (0, 256, -1, True, 100.5, '160'):
        try:
            tool.changed_config(original, count)
        except ValueError:
            pass
        else:
            raise AssertionError(f'Invalid frames accepted: {count!r}')
    conflict = copy.deepcopy(original)
    conflict.data_extended.custom_string_3 = b'Existing user note'.ljust(32, b'\0')
    for reset in (False, True):
        try:
            tool.changed_config(conflict, 160, reset)
        except ValueError:
            pass
        else:
            raise AssertionError('Unrelated custom_string_3 overwritten')
    for value in ('', 'note', tool.KEY, tool.KEY + '0', tool.KEY + '256', tool.KEY + '160x'):
        assert tool.effective_frames(value) == 100

    expected = tool.changed_config(original, 160)
    clients = []
    for config in (original, original, expected):
        client = AsyncMock()
        client.__aenter__.return_value = client
        client.config = config
        clients.append(client)
    with tempfile.TemporaryDirectory() as directory, contextlib.chdir(directory):
        with patch.object(tool, 'OpenDisplayDevice', side_effect=clients):
            await tool.configure('test-address', 160)
        backups = list(Path('build/partial-frame-backups').glob('*.bin'))
        assert len(backups) == 1 and backups[0].read_bytes() == before
        assert stat.S_IMODE(backups[0].stat().st_mode) == 0o600
    assert serialize_config(clients[1].write_config.call_args.args[0]) == serialize_config(expected)
    # Another client changed config while the user was deciding: do not overwrite.
    clients[1].config = expected
    clients[1].write_config.reset_mock()
    with tempfile.TemporaryDirectory() as directory, contextlib.chdir(directory):
        with patch.object(tool, 'OpenDisplayDevice', side_effect=clients[:2]):
            try:
                await tool.configure('test-address', 160)
            except RuntimeError:
                pass
            else:
                raise AssertionError('Concurrent configuration update was overwritten')
    clients[1].write_config.assert_not_called()
    print('Partial frames CLI passed: bounds, field ownership, preservation, private backup and concurrent edits')


if __name__ == '__main__':
    asyncio.run(check())
