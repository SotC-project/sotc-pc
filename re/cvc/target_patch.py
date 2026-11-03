"""Per-colossus player/boss redirect keyed on the AI wrapper slot OR the running script's object.

TGT2/CUR (cvc_base.sh) only redirect while DUAL's slot 0x1593b00 names A or B, i.e. inside the
AI 0x82c080. A colossus's own script (its fiber; 0x2f4e338 = that fiber's object) asks for the
player outside the AI too. Here the slot wins when set, else the fiber object decides:
  0x578810 lock-first-node(list): world+0x228 -> list holding the opponent;
  0x7d1890 GetCurrentBoss: that colossus's boss-table entry.
Prints "TGT4=..." and "CUR4=..." patch lists for the test driver.
"""
import sys
from attack_patch import Stub, jump_patch, md

SLOT, A, B, PLAYERS = 0x1593b00, 0x1593b08, 0x1593b10, 0x1593b18
LIST_HOLDING_B, LIST_HOLDING_A, DEF_A, DEF_B = 0x1593b20, 0x1593b24, 0x1593b28, 0x1593b30
FIBER_OBJECT = 0x2f4e338
# The loser of a knockout (cleared from A/B so its AI is no longer run) and its boss entry: its own
# script keeps getting its boss entry.
LOSER, LOSER_DEF = 0x1593b38, 0x1593b40


def who(stub):
    """rax = the colossus being run: the AI slot if set, else the running script's object."""
    stub.rip('488b05', SLOT)          # mov rax, [slot]
    stub.raw('4885c0')                # test rax, rax
    stub.short('75', 'have')
    stub.rip('488b05', FIBER_OBJECT)  # mov rax, [fiber object]
    stub.label('have')
    stub.raw('4885c0')                # test rax, rax: nobody being run, or a slot cleared at the
    stub.short('74', 'normal')        # end of a fight (0 must not match a cleared A or B)


tgt = Stub(0x1593b80)
tgt.rip('483b3d', PLAYERS)            # cmp rdi, [player list]
tgt.short('75', 'normal')
who(tgt)
tgt.rip('483b05', A)                  # cmp rax, [A]
tgt.short('75', 'notA')
tgt.rip('488d3d', LIST_HOLDING_B)     # lea rdi, [list holding B]
tgt.short('eb', 'normal')
tgt.label('notA')
tgt.rip('483b05', B)                  # cmp rax, [B]
tgt.short('75', 'normal')
tgt.rip('488d3d', LIST_HOLDING_A)     # lea rdi, [list holding A]
tgt.label('normal')
tgt.raw('554889e54156')               # push rbp; mov rbp,rsp; push r14 (0x578810 prologue)
tgt.jmp(0x578816)
tgt.done()

cur = Stub(0x1593e20)
who(cur)
cur.rip('483b05', A)                  # cmp rax, [A]
cur.short('75', 'notA')
cur.rip('488b05', DEF_A)              # mov rax, [A's boss entry]
cur.raw('c3')
cur.label('notA')
cur.rip('483b05', B)                  # cmp rax, [B]
cur.short('75', 'notB')
cur.rip('488b05', DEF_B)              # mov rax, [B's boss entry]
cur.raw('c3')
cur.label('notB')
cur.rip('483b05', LOSER)              # cmp rax, [loser]
cur.short('75', 'normal')
cur.rip('488b05', LOSER_DEF)          # mov rax, [loser's boss entry]
cur.raw('c3')
cur.label('normal')
cur.raw('554889e5bf08000000')         # push rbp; mov rbp,rsp; mov edi,8 (0x7d1890 prologue)
cur.jmp(0x7d1899)
cur.done()

for stub, limit in ((tgt, 0x1593c00), (cur, 0x1593e90)):
    ins = list(md.disasm(bytes(stub.code), stub.start))
    assert sum(i.size for i in ins) == len(stub.code)
    assert stub.here() <= limit, hex(stub.here())
    starts = {i.address for i in ins}
    for i in ins:
        if i.size == 2 and i.mnemonic in ('jne', 'jmp', 'je'):
            assert int(i.op_str, 16) in starts

if __name__ == '__main__':
    if '-v' in sys.argv:
        for stub in (tgt, cur):
            for i in md.disasm(bytes(stub.code), stub.start):
                print(f'{i.address:#x}: {i.mnemonic} {i.op_str}')
            print('--')
    print('TGT4=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in
                             ((tgt.start, tgt.code), (0x578810, jump_patch(0x578810, tgt.start, 6)))))
    print('CUR4=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in
                             ((cur.start, cur.code), (0x7d1890, jump_patch(0x7d1890, cur.start, 9)))))
