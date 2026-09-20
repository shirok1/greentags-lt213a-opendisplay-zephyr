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
def check_stream(data, plain, *, bytewise=False):
    lib.od_zlib_stream_reset(len(plain))
    output = bytearray()
    pos = 0
    while pos < len(data):
        end = min(pos + (1 if bytewise else rng.randrange(1, 245)), len(data))
        chunk = data[pos:end]
        assert lib.od_zlib_stream_push(chunk, len(chunk), end == len(data)) == 0
        while True:
            capacity = (1 if bytewise else rng.randrange(1, 80))
            buf = ct.create_string_buffer(capacity)
            produced = ct.c_size_t()
            status = lib.od_zlib_stream_poll(buf, capacity, ct.byref(produced))
            output += buf.raw[:produced.value]
            assert status >= 0, lib.od_zlib_stream_error()
            if status in (0, 2):
                break
            assert status == 1 and produced.value > 0, status
        pos = end
    assert status == 2 and bytes(output) == plain

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
    check_stream(data, plain)
print("500 zlib streams passed: stored/fixed/dynamic, multi-block, full byte alphabet and random chunk boundaries")

# Alternate table representations with bytewise input/output. Independent raw
# encoders end each fragment at a byte-aligned, nonfinal stored flush block.
parts = [bytes(range(256)) + bytes(range(64)) * 10, bytes(rng.randrange(3) for _ in range(2400)),
         bytes(rng.randrange(256) for _ in range(400)), b"fixed-again" * 80]
wire = bytearray(b"\x18\x19")
for part, level, strategy, kind in zip(parts, (6, 6, 0, 6),
        (zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED),
        (1, 2, 0, 1)):
    c = zlib.compressobj(level=level, wbits=-9, strategy=strategy)
    block = c.compress(part) + c.flush(zlib.Z_SYNC_FLUSH)
    assert (block[0] >> 1) & 3 == kind
    wire += block
plain = b"".join(parts)
wire += b"\x03\x00" + zlib.adler32(plain).to_bytes(4, "big")
assert zlib.decompress(wire) == plain
check_stream(bytes(wire), plain, bytewise=True)

# HLIT=257 is odd: distance length shares a byte with literal/EOB length.
# Only literal A and EOB are present; an all-zero distance tree is legal.
bits = []
def emit(value, width):
    bits.extend((value >> i) & 1 for i in range(width))
emit(1, 1)  # final
emit(2, 2)  # dynamic
emit(0, 5)  # 257 literal/length symbols
emit(0, 5)  # one distance symbol
emit(14, 4)  # 18 code-length symbols
for symbol in (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1):
    emit(int(symbol in (0, 1)), 3)
for symbol in range(258):
    emit(int(symbol in (65, 256)), 1)
plain = b"A" * 37
for _ in plain:
    emit(0, 1)
emit(1, 1)  # EOB
body = bytearray((len(bits) + 7) // 8)
for i, bit in enumerate(bits):
    body[i // 8] |= bit << (i % 8)
wire = b"\x18\x19" + body + zlib.adler32(plain).to_bytes(4, "big")
assert zlib.decompress(wire) == plain
check_stream(bytes(wire), plain, bytewise=True)
print("Canonical lookup passed: mixed block types, bytewise resume, odd HLIT and empty distance tree")

# A complete canonical tree with codes at every depth through 15 bits.
# The last literal and EOB use 15 bits; the distance alphabet is unused.
bits.clear()
emit(1, 1)
emit(2, 2)
emit(0, 5)
emit(0, 5)
emit(15, 4)  # all 19 code-length symbols
for symbol in (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15):
    emit(4 if symbol < 16 else 0, 3)
lengths = [0] * 258
for symbol in range(65, 80):
    lengths[symbol] = symbol - 64
lengths[256] = 15
for length in lengths:
    bits.extend((length >> i) & 1 for i in range(3, -1, -1))
counts = [lengths.count(n) if n else 0 for n in range(16)]
next_code = [0] * 16
for n in range(1, 16):
    next_code[n] = (next_code[n - 1] + counts[n - 1]) << 1
codes = {}
for symbol, length in enumerate(lengths):
    if length:
        codes[symbol] = (next_code[length], length)
        next_code[length] += 1
plain = bytes(range(65, 80)) + bytes(range(79, 64, -1))
for symbol in [*plain, 256]:
    code, length = codes[symbol]
    bits.extend((code >> i) & 1 for i in range(length - 1, -1, -1))
body = bytearray((len(bits) + 7) // 8)
for i, bit in enumerate(bits):
    body[i // 8] |= bit << (i % 8)
wire = b"\x18\x19" + body + zlib.adler32(plain).to_bytes(4, "big")
assert zlib.decompress(wire) == plain
check_stream(bytes(wire), plain, bytewise=True)
print("Canonical 15-bit codes passed with one-byte input/output")

# Narrow state fields must retain rejection behavior at their wire limits.
def packed_bits():
    body = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        body[i // 8] |= bit << (i % 8)
    return b"\x18\x19" + body

def check_rejected(data, message):
    lib.od_zlib_stream_reset(1)
    assert lib.od_zlib_stream_push(data, len(data), True) == 0
    out = ct.create_string_buffer(64)
    produced = ct.c_size_t()
    assert lib.od_zlib_stream_poll(out, len(out), ct.byref(produced)) < 0
    assert message in lib.od_zlib_stream_error()

for hlit in (30, 31):  # Encodes forbidden dynamic HLIT 287/288.
    bits.clear()
    emit(1, 1); emit(2, 2); emit(hlit, 5); emit(0, 5); emit(0, 4)
    check_rejected(packed_bits(), b"invalid dynamic tree sizes")

bits.clear()
emit(1, 1); emit(2, 2); emit(0, 5); emit(0, 5); emit(0, 4)
for length in (0, 0, 1, 1):  # Alphabet 0/18; symbol 18 encodes 11..138 zeros.
    emit(length, 3)
for _ in range(2):
    emit(1, 1); emit(127, 7)  # 276 lengths cannot fit HLIT+HDIST=258.
check_rejected(packed_bits(), b"dynamic repeat exceeds tree size")

bits.clear()
emit(1, 1); emit(2, 2); emit(0, 5); emit(0, 5); emit(15, 4)
for symbol in (16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15):
    emit(int(symbol in (0, 15)), 3)
for symbol in range(258):
    emit(int(symbol in (65, 256)), 1)  # Incomplete 15-bit literal/EOB tree.
emit(0xffff, 16)  # Must reject at depth 16 without overflowing signed state.
check_rejected(packed_bits(), b"invalid huffman code")
print("Bounded state rejection passed: HLIT 287/288, repeat 138 overflow and invalid 16-bit code")

# The reusable inflater also permits a 15-bit window; cover index wrap there.
subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-fsanitize=undefined",
    "-Wall", "-Wextra", "-Werror", "-DOPENDISPLAY_ZLIB_WINDOW_BITS=15",
    "-Ithird_party/uzlib", "third_party/uzlib/od_zlib_stream.c", "third_party/uzlib/adler32.c",
    "-o", "build/test-zlib-window15.so"], check=True)
small_window = lib
lib = ct.CDLL(str(Path("build/test-zlib-window15.so").resolve()))
for name in ("od_zlib_stream_reset", "od_zlib_stream_push", "od_zlib_stream_poll", "od_zlib_stream_error"):
    getattr(lib, name).argtypes = getattr(small_window, name).argtypes
    getattr(lib, name).restype = getattr(small_window, name).restype
plain = random.Random(1463).randbytes(16000) * 3
check_stream(zlib.compress(plain, wbits=15), plain)
lib = small_window
print("15-bit window passed: long back-references and index wrap beyond 32768 output bytes")
