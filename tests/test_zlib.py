"""Cross-check packed Huffman decoding across streaming boundaries."""
from pathlib import Path
import ctypes as ct
import os
import random
import subprocess
import zlib

os.chdir(Path(__file__).resolve().parents[1])
subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-fsanitize=undefined",
    "-Wall", "-Wextra", "-Werror", "-Ithird_party/uzlib",
    "third_party/uzlib/od_zlib_stream.c", "third_party/uzlib/adler32.c",
    "-o", "build/test-zlib.so"], check=True)
lib = ct.CDLL(str(Path("build/test-zlib.so").resolve()))
lib.od_zlib_stream_reset.argtypes = [ct.c_uint32]
lib.od_zlib_stream_push.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_bool]
lib.od_zlib_stream_poll.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t)]
lib.od_zlib_stream_error.restype = ct.c_char_p
rng = random.Random(948)
for n in range(500):
    size = 0 if n == 0 else rng.randrange(1, 6000)
    alphabet = (2, 16, 256)[n % 3]
    plain = bytes(rng.randrange(alphabet) for _ in range(size))
    compressor = zlib.compressobj(level=n % 10, wbits=9,
        strategy=(zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY)[n % 3])
    if n % 5 == 0 and size:
        # Several blocks reuse the same packed tree storage in one stream.
        split = size // 2
        data = compressor.compress(plain[:split]) + compressor.flush(zlib.Z_SYNC_FLUSH)
        data += compressor.compress(plain[split:]) + compressor.flush()
    else:
        data = compressor.compress(plain) + compressor.flush()
    lib.od_zlib_stream_reset(size)
    output = bytearray()
    pos = 0
    while pos < len(data):
        end = min(pos + rng.randrange(1, 245), len(data))
        chunk = data[pos:end]
        assert lib.od_zlib_stream_push(chunk, len(chunk), end == len(data)) == 0
        while True:
            capacity = rng.randrange(1, 80)
            buf = ct.create_string_buffer(capacity)
            produced = ct.c_size_t()
            status = lib.od_zlib_stream_poll(buf, capacity, ct.byref(produced))
            output += buf.raw[:produced.value]
            assert status >= 0, (n, lib.od_zlib_stream_error())
            if status in (0, 2):
                break
            assert status == 1 and produced.value > 0, (n, status)
        pos = end
    assert status == 2 and bytes(output) == plain, n
print("500 zlib streams passed: stored/fixed/dynamic, multi-block, full byte alphabet and random chunk boundaries")
