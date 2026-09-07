"""Cross-check the C wire implementation against independent OpenSSL primitives."""
import ctypes as ct
from pathlib import Path
import subprocess
import os
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.cmac import CMAC
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

root = Path(__file__).resolve().parents[1]
tiny = root / ".deps/modules/crypto/tinycrypt/lib"
library = root / "build/test-security.so"
subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
    "-fsanitize=undefined", "-Isrc", f"-I{tiny / 'include'}", "src/security.c", "src/config_store.c",
    "tests/security_bridge.c", *[str(tiny / f"source/{name}.c") for name in ("aes_encrypt", "cmac_mode", "ccm_mode", "utils")],
    "-o", str(library)], check=True, cwd=root)
lib = ct.CDLL(str(library))
lib.setup.argtypes = [ct.c_void_p, ct.c_size_t]
lib.auth.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32, ct.c_void_p]
lib.decrypt.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_uint32]
lib.encrypt.argtypes = [ct.c_void_p, ct.c_size_t, ct.c_void_p, ct.c_size_t]
lib.expire.argtypes = [ct.c_uint32]
config = (root / "build/secure-config.bin").read_bytes()
assert lib.setup(config, len(config)) == 0

def cmac(key, data):
    c = CMAC(algorithms.AES(key)); c.update(data); return c.finalize()

def auth(payload, now=1000):
    out = ct.create_string_buffer(23)
    count = lib.auth(payload, len(payload), now, out)
    return out.raw[:count]

def decrypt(packet, now=1001):
    data = ct.create_string_buffer(packet)
    count = lib.decrypt(data, len(packet), now)
    return count, data.raw[:max(count, 0)]

challenge = auth(b"\0")
assert len(challenge) == 23 and challenge[:3] == b"\0\x50\0"
server, device = challenge[3:19], challenge[19:23]
master, client = bytes(range(16)), bytes(range(0x80, 0x90))
assert device == bytes.fromhex("a1b2c3d4")
intermediate = cmac(master, b"OpenDisplay session\0" + device + client + server + b"\0\x80")
encryptor = Cipher(algorithms.AES(master), modes.ECB()).encryptor()
session = encryptor.update((1).to_bytes(8, "big") + intermediate[:8]) + encryptor.finalize()
session_id = cmac(session, client + server)[:8]
proof = cmac(master, server + client + device)
result = auth(client + proof)
assert result == b"\0\x50\0" + cmac(session, server + client + device)
assert lib.authenticated()

def packet(counter, payload=b"hello", opcode=b"\0\x71"):
    nonce = session_id + counter.to_bytes(8, "big")
    return opcode + nonce + AESCCM(session, tag_length=12).encrypt(nonce[3:], bytes([len(payload)]) + payload, opcode)

good = packet(0)
bad = good[:-1] + bytes([good[-1] ^ 1])
assert decrypt(bad)[0] < 0
assert decrypt(good) == (7, b"\0\x71hello")  # bad tag did not consume counter 0
assert decrypt(good)[0] < 0
assert decrypt(packet(2))[0] == 7
assert decrypt(packet(1))[0] == 7  # out-of-order inside replay window
assert decrypt(packet(1))[0] < 0
assert decrypt(packet(1 << 63))[0] < 0  # TX and RX domains never overlap
assert decrypt(packet(100))[0] == 7
assert decrypt(packet(3))[0] < 0
changed = bytearray(packet(101)); changed[1] ^= 1
assert decrypt(bytes(changed))[0] < 0
assert decrypt(packet(101, bytes(range(200))))[0] == 202

out = ct.create_string_buffer(244)
plain = b"\0\x71"
size = lib.encrypt(plain, len(plain), out, len(out))
wire = out.raw[:size]
assert size == 31 and wire[:2] == plain
assert int.from_bytes(wire[10:18], "big") == 1 << 63
assert AESCCM(session, tag_length=12).decrypt(wire[5:18], wire[18:], wire[:2]) == b"\0"
# Valid traffic immediately before expiry must not renew the session.
assert lib.expire(60999) == 0
assert decrypt(packet(102), now=60999)[0] == 7
assert lib.expire(61000) == 1 and not lib.authenticated()
assert decrypt(packet(102))[0] < 0

# Proof without a challenge, wrong proof, challenge replay and expiry.
assert auth(client + proof, 70000)[2] == 255
new = auth(b"\0", 70000)
assert auth(client + bytes(16), 70000)[2] == 1
assert auth(client + cmac(master, new[3:19] + client + device), 70000)[2] == 255
auth(b"\0", 140000)
assert auth(client + bytes(16), 170000)[2] == 255

# Reconnects do not reset the rate limiter.
for i in range(10):
    assert auth(b"\0", 240000)[2] == 0
    lib.disconnect_session()
assert auth(b"\0", 240000)[2] == 4
assert auth(b"\0", 300000)[2] == 0
for n in range(246):
    assert decrypt(bytes(n))[0] < 0
print("Crypto interoperability passed: CMAC KDF, mutual proofs, CCM, tamper/replay rejection, expiry and rate limiting")
