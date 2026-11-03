"""Emit a register-preserving probe for actual transform rebuild arguments."""
import struct
import capstone
start = 0x1593d80
code = bytearray()
def rip(op, target):
    code.extend(bytes.fromhex(op))
    code.extend(struct.pack('<i', target - (start + len(code) + 4)))
code.extend(bytes.fromhex('9c50'))  # Preserve flags and scratch register.
rip('488b05', 0x1593b08)
code.extend(bytes.fromhex('480530010000483b8750010000'))
code.extend(bytes.fromhex('7515'))  # Record only when position pointer is A+0x130.
rip('48893d', 0x1593e90)
rip('488935', 0x1593e98)
rip('488915', 0x1593ea0)
code.extend(bytes.fromhex('589d554889e54157'))
rip('e9', 0x6611c6)
hook = b'\xe9' + struct.pack('<i', start - (0x6611c0 + 5)) + b'\x90'
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
assert sum(i.size for i in md.disasm(code, start)) == len(code)
assert start + len(code) < 0x1593e00
print(f'patch={start:x}:{code.hex()},patch=6611c0:{hook.hex()}')
