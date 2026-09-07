"""Full baseline then a small partial update through the official package.

uv run --locked python tests/test_partial_hardware.py DEVICE BEFORE AFTER
Images must already have native 104x212 orientation.
"""
import argparse
import asyncio
import logging
from pathlib import Path
from time import monotonic

from opendisplay import OpenDisplayDevice
from opendisplay.partial import PartialState
from PIL import Image, ImageChops


async def check(address, before_path, after_path, white_first=False, invert_first=False):
    with Image.open(before_path) as source:
        before = source.convert('RGB')
    with Image.open(after_path) as source:
        after = source.convert('RGB')
    assert before.size == after.size == (104, 212)
    bounds = ImageChops.difference(before, after).getbbox()
    assert bounds and (bounds[2] - bounds[0]) * (bounds[3] - bounds[1]) < 104 * 212 // 4
    print(f'Changed native rectangle: {bounds}', flush=True)
    stages = [('target', after)]
    if white_first or invert_first:
        intermediate = before.copy()
        rectangle = (bounds[0] // 8 * 8, bounds[1], (bounds[2] + 7) // 8 * 8, bounds[3])
        fill = ImageChops.invert(after.crop(rectangle)) if invert_first else 'white'
        intermediate.paste(fill, rectangle)
        stages.insert(0, ('inverted-target' if invert_first else 'white', intermediate))
    state = PartialState()
    async with OpenDisplayDevice(mac_address=address, timeout=20) as device:
        # Compressed full upload has explicit END, establishing the etag needed
        # by the package's subsequent partial update. No transport overrides.
        processed = await device.upload_image(before, compress=True, state=state)
        assert processed.convert('1').tobytes() == before.convert('1').tobytes()
        assert state.etag, 'Baseline did not establish an etag'
        print('Full baseline refreshed; next operations are partial updates.', flush=True)
        total_started = monotonic()
        for name, target in stages:
            old_etag = state.etag
            progress = []
            started = monotonic()
            processed = await device.upload_image(target, state=state,
                progress_callback=lambda sent, total: progress.append((sent, total)))
            assert device._pipe_partial_supported, 'Partial PIPE was not negotiated'
            assert state.etag and state.etag != old_etag
            assert processed.convert('1').tobytes() == target.convert('1').tobytes()
            assert progress and max(total for _, total in progress) < 2756
            Path('build/partial-last-state.bin').write_bytes(state.to_bytes())
            print(f'{name}: partial refresh confirmed in {monotonic() - started:.2f}s; '
                  f'transfer={progress[-1]}; etag updated.', flush=True)
        print(f'Partial sequence completed in {monotonic() - total_started:.2f}s', flush=True)



if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('address')
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    strategy = parser.add_mutually_exclusive_group()
    strategy.add_argument('--white-first', action='store_true', help='Partially clear the changed rectangle before writing target')
    strategy.add_argument('--invert-first', action='store_true', help='Invert the target rectangle before writing target')
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO)
    logging.getLogger('opendisplay.device').setLevel(logging.DEBUG)
    asyncio.run(check(args.address, args.before, args.after, args.white_first, args.invert_first))
