"""Host protocol tests, sanitizer checks, and configuration-container validation."""
from pathlib import Path
import binascii
import os
import re
import subprocess
import sys
import zlib
import random
from generate_config import config_bytes

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
(ROOT / "build").mkdir(exist_ok=True)
raw = bytes(random.Random(123).choices(range(16), k=2756))
Path("build/fixture.raw").write_bytes(raw)
for name, level, strategy in [("dynamic", 6, zlib.Z_DEFAULT_STRATEGY), ("fixed", 6, zlib.Z_FIXED), ("stored", 0, zlib.Z_DEFAULT_STRATEGY)]:
    compressor = zlib.compressobj(level=level, wbits=9, strategy=strategy)
    Path(f"build/{name}.zlib").write_bytes(compressor.compress(raw) + compressor.flush())
config = bytearray(config_bytes()[:-2])
security = bytearray(64)
security[0] = 1
security[1:17] = bytes(range(16))
security[17:19] = (60).to_bytes(2, "little")
security[20] = 255
config += b"\0\x27" + security + b"\0\x2c" + bytes(288)
config[:2] = b"\0\0"
config += binascii.crc_hqx(config, 0xffff).to_bytes(2, "little")
config[:2] = len(config).to_bytes(2, "little")
Path("build/secure-config.bin").write_bytes(config)
subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-g", "-fsanitize=address,undefined", "-Isrc", "-Ithird_party/uzlib", "src/protocol.c",
                "src/config_store.c", "third_party/uzlib/od_zlib_stream.c", "third_party/uzlib/adler32.c",
                "tests/test_protocol.c", "-o", "build/test-protocol"], check=True)
subprocess.run(["build/test-protocol"], check=True)
actual = (ROOT / "build/test-config.bin").read_bytes()
expected = config_bytes()
assert actual == expected
assert actual == bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", Path("src/config.inc").read_text()))
assert len(actual) == int.from_bytes(actual[:2], "little") == 133
assert binascii.crc_hqx(b"\0\0" + actual[2:-2], 0xFFFF) == int.from_bytes(actual[-2:], "little")
offset = 3
for kind, size in [(1, 22), (2, 22), (4, 30), (32, 46)]:
    assert actual[offset:offset + 2] == bytes([0, kind])
    offset += 2 + size
assert offset == len(actual) - 2
print("Configuration bytes, packet IDs, sizes and CRC passed")
subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-g", "-fsanitize=address,undefined", "-Isrc", "src/config_store.c",
                "tests/test_storage.c", "-o", "build/test-storage"], check=True)
subprocess.run(["build/test-storage"], check=True)
subprocess.run([sys.executable, "tests/test_security.py"], check=True)
subprocess.run([sys.executable, "scripts/test_epd.py"], check=True)
subprocess.run([sys.executable, "scripts/test_telemetry.py"], check=True)
subprocess.run([sys.executable, "tests/test_upload.py"], check=True)
