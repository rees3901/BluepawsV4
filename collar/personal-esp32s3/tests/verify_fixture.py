"""Independent HMAC/packet verification of the native C++ fixture."""
import hashlib
import hmac
import struct
import subprocess
import sys
from pathlib import Path

exe = Path(sys.argv[1]).resolve()
result = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
packet = bytes.fromhex(result.stdout.splitlines()[0])
assert packet[-8:] == hmac.new(bytes(range(32)), packet[:-8], hashlib.sha256).digest()[:8]
assert packet[0] == 2 and len(packet) == 44
assert struct.unpack_from('<HHHI', packet, 1) == (3001, 48, 65432, 1791061200)
assert struct.unpack_from('<ii', packet, 14) == (519059786, -22394294)
assert packet[29:32] == b'\0\0\x04'
assert packet[32:36] == b'\x04\x02\0\x01'
print(result.stdout.splitlines()[-1])
print('PASS: independent Python HMAC and V4 wire-layout verification')
