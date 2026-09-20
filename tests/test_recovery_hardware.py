"""Opt-in nRF51822 watchdog fault injection (halts normal work for ~3 minutes).

uv run --locked python tests/test_recovery_hardware.py DEVICE
Requires probe-rs, OpenOCD and the exact flashed build/zephyr/zephyr.elf.
Does not change Flash/configuration. Uses the last 16 bytes of unallocated RAM.
"""
import argparse
import asyncio
from pathlib import Path
import subprocess
import time
from elftools.elf.elffile import ELFFile
from opendisplay import OpenDisplayDevice

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('address')
a = p.parse_args()
elf_path = Path('build/zephyr/zephyr.elf')
with elf_path.open('rb') as f:
    elf = ELFFile(f)
    ram_end = max(s['p_vaddr'] + s['p_memsz'] for s in elf.iter_segments()
                  if s['p_type'] == 'PT_LOAD' and s['p_vaddr'] >= 0x20000000)
assert ram_end <= 0x20003ff0, 'No free scratch RAM for injected loop'
probe = ['probe-rs', 'read', '--chip', 'nRF51822_xxAB', '--speed', '1000', 'b32']
def read(address, count=1):
    result = subprocess.check_output([*probe, hex(address), str(count)], text=True)
    return [int(word, 16) for word in result.split(':', 1)[1].split()]
assert read(0x40010400)[0] == 1, 'Watchdog not running'
assert read(0x40010504, 3) == [180 * 32768 - 1, 1, 1], 'Wrong timeout/channel/sleep policy'
# End the debug session before the watchdog resets. Interrupting a live radio
# after reboot merely to detach GDB can itself break advertising/controller timing.
with open('build/watchdog-injection.log', 'w') as log:
    subprocess.run(['openocd', '-f', 'interface/cmsis-dap.cfg',
                    '-c', 'transport select swd', '-f', 'target/nrf51.cfg',
                    '-c', 'adapter speed 1000', '-c',
                    'init; halt; mww 0x40000400 0xffffffff; '
                    'mww 0x20003ff0 0xe7feb672; '
                    'mww 0x40010600 0x6e524635; resume 0x20003ff0; shutdown'],
                   stdout=log, stderr=log, check=True, timeout=15)
assert not read(0xe000edf0)[0] & (1 << 17), 'CPU remained halted after injection'
assert read(0x40000400)[0] == 0, 'Unexpected reset before watchdog interval'
print('CPU running CPSID I + infinite loop; debugger detached before watchdog reset', flush=True)
for seconds in (60, 60, 60, 5):
    time.sleep(seconds)
    print(f'Waited another {seconds}s', flush=True)
reason = read(0x40000400)[0]
assert reason & 2, f'No hardware watchdog reset: RESETREAS={reason:#x}'
assert read(0x40010400)[0] == 1
async def verify():
    async with OpenDisplayDevice(mac_address=a.address, timeout=20) as d:
        assert not d.config.security_config or not d.config.security_config.encryption_enabled
        print('Watchdog RESETREAS confirmed; BLE config/version accessible:', await d.read_firmware_version(), flush=True)
asyncio.run(verify())
