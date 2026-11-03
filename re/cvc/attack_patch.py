"""Patches routing each colossus's attack collision check at the other colossus.

0x82ede0 is the AI command that tests an attack volume (two bones) against the list at
world+0x138 through 0x6db4b0. While the attacker is colossus A or B (cave slots 0x1593b08 /
0x1593b10) the list becomes the cvctarget one-entry list holding the opponent (0x1593b20 for A,
0x1593b24 for B). Counters (u32): 0x1593ec0 checks by A, 0x1593ec4 by B, 0x1593ec8 hits by A,
0x1593ecc hits by B. Prints the test-driver patch list.
"""
import struct
import capstone

SLOT_A, SLOT_B = 0x1593b08, 0x1593b10
LIST_HOLDING_B, LIST_HOLDING_A = 0x1593b20, 0x1593b24
WORLD = 0x15fe570
CHECKS_A, CHECKS_B, HITS_A, HITS_B = 0x1593ec0, 0x1593ec4, 0x1593ec8, 0x1593ecc
ACTIVE = 0x1593ed0  # byte: an A/B attack check is running (set by redirect, cleared after)


class Stub:
    def __init__(self, start):
        self.start, self.code = start, bytearray()

    def here(self):
        return self.start + len(self.code)

    def raw(self, hexs):
        self.code += bytes.fromhex(hexs)

    def rip(self, op, target, tail=b''):
        # op bytes, then rel32 to target measured from the end of the instruction.
        self.code += bytes.fromhex(op)
        end = self.here() + 4 + len(tail)
        self.code += struct.pack('<i', target - end) + tail

    def jmp(self, target):
        self.rip('e9', target)

    def short(self, op, label):
        # Short conditional/unconditional jump to a label bound later.
        self.code += bytes.fromhex(op) + bytes(1)
        self.fixups = getattr(self, 'fixups', []) + [(len(self.code), label)]

    def label(self, name):
        self.labels = getattr(self, 'labels', {})
        self.labels[name] = len(self.code)

    def done(self):
        for end, name in getattr(self, 'fixups', []):
            delta = self.labels[name] - end
            assert -128 <= delta < 128
            self.code[end - 1] = delta & 0xff


def jump_patch(at, stub, length):
    body = b'\xe9' + struct.pack('<i', stub - (at + 5))
    return body + b'\x90' * (length - len(body))


# Entry counter: 0x82ede0 push rbp / mov rbp,rsp / push r14 / push rbx (7 bytes).
entry = Stub(0x1593d80)
entry.rip('483b3d', SLOT_A)              # cmp rdi, [A]
entry.short('75', 'notA')
entry.rip('ff05', CHECKS_A)              # inc dword [checksA]
entry.short('eb', 'prologue')
entry.label('notA')
entry.rip('483b3d', SLOT_B)              # cmp rdi, [B]
entry.short('75', 'prologue')
entry.rip('ff05', CHECKS_B)              # inc dword [checksB]
entry.label('prologue')
entry.raw('554889e5415653')              # push rbp; mov rbp,rsp; push r14; push rbx
entry.jmp(0x82ede7)
entry.done()

# List redirect: 0x82ee5f mov ecx,0x138 / lea rax,[rbp-0x60] / add rcx,[rip->world] (16 bytes).
redirect = Stub(0x1593db0)
redirect.raw('488d45a0')                 # lea rax, [rbp-0x60]
redirect.rip('488b0d', WORLD)            # mov rcx, [world]
redirect.raw('4881c138010000')           # add rcx, 0x138
redirect.rip('483b1d', SLOT_A)           # cmp rbx, [A]
redirect.short('75', 'notA')
redirect.rip('488d0d', LIST_HOLDING_B)   # lea rcx, [list holding B]
redirect.rip('c605', ACTIVE, b'\x01')    # mov byte [active], 1
redirect.short('eb', 'back')
redirect.label('notA')
redirect.rip('483b1d', SLOT_B)           # cmp rbx, [B]
redirect.short('75', 'back')
redirect.rip('488d0d', LIST_HOLDING_A)   # lea rcx, [list holding A]
redirect.rip('c605', ACTIVE, b'\x01')    # mov byte [active], 1
redirect.label('back')
redirect.jmp(0x82ee6f)
redirect.done()

# Hit record: 0x82ee78 test al,al / je 0x82eecb / mov rcx,[rbp+0x20] (8 bytes).
hit = Stub(0x1593f80)
hit.rip('c605', ACTIVE, b'\x00')         # mov byte [active], 0 (flags untouched)
hit.raw('84c0')                          # test al, al
hit.short('75', 'hit')
hit.jmp(0x82eecb)
hit.label('hit')
hit.rip('483b1d', SLOT_A)                # cmp rbx, [A]
hit.short('75', 'notA')
hit.rip('ff05', HITS_A)                  # inc dword [hitsA]
hit.short('eb', 'tail')
hit.label('notA')
hit.rip('483b1d', SLOT_B)                # cmp rbx, [B]
hit.short('75', 'tail')
hit.rip('ff05', HITS_B)                  # inc dword [hitsB]
hit.label('tail')
hit.raw('488b4d20')                      # mov rcx, [rbp+0x20]
hit.jmp(0x82ee80)
hit.done()

# Surface filter 0x82eef0 (push rbp / mov rbp,rsp / push r14, 6 bytes): the attack's category
# mask matches Wander's body; while an A/B check runs, any surface of the opponent counts.
accept = Stub(0x1593fc0)
accept.rip('803d', ACTIVE, b'\x00')      # cmp byte [active], 0
accept.short('74', 'normal')
accept.raw('b001c3')                     # mov al, 1; ret
accept.label('normal')
accept.raw('554889e54156')               # push rbp; mov rbp,rsp; push r14
accept.jmp(0x82eef6)
accept.done()

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def check(stub, limit):
    ins = list(md.disasm(bytes(stub.code), stub.start))
    assert sum(i.size for i in ins) == len(stub.code), hex(stub.start)
    assert stub.here() <= limit, (hex(stub.here()), hex(limit))
    return ins


for stub, limit in ((entry, 0x1593db0), (redirect, 0x1593e00), (hit, 0x1593fc0),
                    (accept, 0x1594000)):
    ins = check(stub, limit)
    # Every short jump must land on an instruction start inside the stub.
    starts = {i.address for i in ins}
    for i in ins:
        if i.mnemonic in ('jne', 'jmp', 'je') and i.size == 2:
            assert int(i.op_str, 16) in starts, (hex(i.address), i.op_str)

if __name__ == '__main__':
    import sys
    if '-v' in sys.argv:
        for stub in (entry, redirect, hit, accept):
            for i in md.disasm(bytes(stub.code), stub.start):
                print(f'{i.address:#x}: {i.mnemonic} {i.op_str}')
            print('--')
    patches = [(entry.start, entry.code), (redirect.start, redirect.code), (hit.start, hit.code),
               (accept.start, accept.code),
               (0x82eef0, jump_patch(0x82eef0, accept.start, 6)),
               (0x82ede0, jump_patch(0x82ede0, entry.start, 7)),
               (0x82ee5f, jump_patch(0x82ee5f, redirect.start, 16)),
               (0x82ee78, jump_patch(0x82ee78, hit.start, 8))]
    print(','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in patches))
