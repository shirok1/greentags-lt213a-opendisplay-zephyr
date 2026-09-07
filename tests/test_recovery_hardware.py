"""Opt-in nRF51822 watchdog fault injection (halts normal work for ~3 minutes).

uv run --locked python tests/test_recovery_hardware.py DEVICE
Requires probe-rs, arm-none-eabi-gdb and the exact flashed build/zephyr/zephyr.elf.
Does not change Flash/configuration. Uses the last 16 bytes of unallocated RAM.
"""
import argparse
import asyncio
from pathlib import Path
import signal
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
with open('build/watchdog-gdb.log', 'w') as log:
    server = subprocess.Popen(['probe-rs', 'gdb', '--chip', 'nRF51822_xxAB', '--speed', '1000'], stdout=log, stderr=log)
    debugger = None
    try:
        time.sleep(1)
        # Execute CPSID I followed by B .; GDB does not expose PRIMASK on this
        # probe. Keep an explicit continue active: detach is not proof of run.
        commands = ['set pagination off', 'target remote :1337', 'set mem inaccessible-by-default off',
                    'set {unsigned int}0x40000400 = 0xffffffff',
                    'set {unsigned int}0x20003ff0 = 0xe7feb672',
                    'set $pc = 0x20003ff0',
                    'set {unsigned int}0x40010600 = 0x6e524635', 'continue']
        args = ['arm-none-eabi-gdb', '-q', str(elf_path)]
        for command in commands:
            args += ['-ex', command]
        debugger = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log, text=True)
        time.sleep(2)
        assert debugger.poll() is None
        output = Path('build/watchdog-gdb.log').read_text()
        assert 'Continuing.' in output and 'Cannot access' not in output, 'Fault injection did not start'
        print('Explicit GDB continue: CPSID I + infinite loop; watchdog must reset CPU', flush=True)
        for seconds in (60, 60, 60, 5):
            time.sleep(seconds)
            print(f'Waited another {seconds}s', flush=True)
        debugger.send_signal(signal.SIGINT)
        debugger.communicate('set confirm off\ndetach\nquit\n', timeout=15)
    finally:
        if debugger and debugger.poll() is None:
            debugger.kill()
            debugger.wait(timeout=10)
        server.terminate()
        server.wait(timeout=10)
reason = read(0x40000400)[0]
assert reason & 2, f'No hardware watchdog reset: RESETREAS={reason:#x}'
assert read(0x40010400)[0] == 1
async def verify():
    async with OpenDisplayDevice(mac_address=a.address, timeout=20) as d:
        assert not d.config.security_config or not d.config.security_config.encryption_enabled
        print('Watchdog RESETREAS confirmed; BLE config/version accessible:', await d.read_firmware_version(), flush=True)
asyncio.run(verify())
