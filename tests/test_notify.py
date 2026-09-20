"""Exercise the actual main.c notification adapter with a deterministic scheduler.

Zephyr's ATT allocator/radio are not simulated; hardware tests cover those.
Extract functions verbatim so the fixture tests production timeout/ticket logic.
"""
from pathlib import Path
import os
import subprocess

source = Path('src/main.c').read_text()
state = source[source.index('K_SEM_DEFINE(notify_done,'):source.index('static uint8_t msd')]
functions = source[source.index('static bool live('):source.index('static int send_response(')]
Path('build/notify-under-test.inc').write_text(state + functions)
subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-g', '-fsanitize=address,undefined', '-Ibuild', 'tests/test_notify.c',
                '-o', 'build/test-notify'], check=True)
subprocess.run(['build/test-notify'], check=True)
