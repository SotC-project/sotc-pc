"""Colossus-vs-colossus hits from the game's own attack-surface test.

Wander is hurt when a capsule between two of his bones meets an attack surface: 0x6db4b0(out,
bone, bone, list, body, filter 0x71f2a0, 0) at 0x71d920 (player_damageReaction). The same call,
with the victim colossus as body and a one-entry list holding the attacker (the cvctarget lists),
tells when an attack of one colossus touches the other.

(That test never reported a hit; QUERY is now an empty stub, see below.)
DUAL3 replaces DUAL (cvc_base.sh): after running both AIs it calls QUERY, which ran the test for
segments of each victim's skeleton listed in a host-built table (cvcbones=, pointer at 0x1593dc0)
and counts: 0x1593ec0 tests, the table's counters 0x1593ec8 hits BY A (on B), 0x1593ecc hits BY B
(on A) -- the cvchits= counters. Bone ids per 0x616470 (-2 = the character's position). Result
buffer 0x1593d80.
Prints DUAL3=... (DUAL3 includes QUERY and the 0x82c080 hook).
"""
import sys
from attack_patch import Stub, jump_patch, md

SLOT, A, B = 0x1593b00, 0x1593b08, 0x1593b10
LIST_HOLDING_B, LIST_HOLDING_A = 0x1593b20, 0x1593b24
TESTS_A, TESTS_B, HITS_BY_A, HITS_BY_B = 0x1593ec0, 0x1593ec4, 0x1593ec8, 0x1593ecc
OUT, TABLE, TESTS = 0x1593d80, 0x1593dc0, 0x1593ec0
QUERY_AT, AI_TRAMPOLINE = 0x1593a60, 0x1593c80  # old S2/CUR area, unused with TGT4/CUR4
TEST, FILTER = 0x6db4b0, 0x71f2a0

query = Stub(QUERY_AT)
# The table-driven contact test never hit (hits come from cvcbrawl's skeleton contact): QUERY only
# returns now, and the rest of its room (0x1593a61..0x1593b00) holds camera_patch.py's stubs.
query.raw('c3')                           # ret
query.done()

# DUAL with a call to QUERY once both AIs have run (same as cvc_base.sh DUAL otherwise).
dual = Stub(0x1593c00)
dual.raw('534154')                        # push rbx; push r12
dual.rip('ff35', SLOT)                    # push qword [slot]
dual.rip('488b1d', A)                     # mov rbx, [A]
dual.raw('4885db')                        # test rbx, rbx
dual.short('74', 'single')
dual.raw('4839df')                        # cmp rdi, rbx
dual.short('74', 'both')
dual.rip('483b3d', B)                     # cmp rdi, [B]
dual.short('75', 'single')
dual.label('both')
dual.rip('488b3d', A)                     # mov rdi, [A]
dual.rip('48893d', SLOT)                  # mov [slot], rdi
dual.rip('e8', AI_TRAMPOLINE)             # call AI (A)
dual.rip('488b3d', B)                     # mov rdi, [B]
dual.rip('48893d', SLOT)                  # mov [slot], rdi
dual.rip('e8', AI_TRAMPOLINE)             # call AI (B)
dual.rip('c705', SLOT, b'\x00\x00\x00\x00')  # mov dword [slot], 0 (low half; high half below)
dual.rip('c705', SLOT + 4, b'\x00\x00\x00\x00')
dual.rip('e8', QUERY_AT)                  # call QUERY
dual.short('eb', 'done')
dual.label('single')
dual.rip('e8', AI_TRAMPOLINE)             # call AI (anyone else)
dual.label('done')
dual.rip('8f05', SLOT)                    # pop qword [slot]
dual.raw('415c5bc3')                      # pop r12; pop rbx; ret
dual.done()

# A knocked-out guest colossus plays its death motion (sdl_window.cpp CvcRayWork); its own script
# loop would run its AI on top of it (it walks off), so the AI is not run for it (EXCLUDED).
# A second slot (EXCLUDED_2) is for a colossus playing an attack or a hit reaction (cvcbrawl): its
# AI would replace the move with its own next frame.
EXCLUDED, EXCLUDED_2 = 0x1593b48, 0x1593ae0
trampoline = Stub(AI_TRAMPOLINE)
trampoline.raw('4885ff')                  # test rdi, rdi: a slot cleared after a defeat
trampoline.short('74', 'skip')
trampoline.rip('483b3d', EXCLUDED)        # cmp rdi, [excluded]
trampoline.short('74', 'skip')
trampoline.rip('483b3d', EXCLUDED_2)      # cmp rdi, [excluded 2]
trampoline.short('74', 'skip')
trampoline.rip('e9', 0x1593fd0)           # the checks continue in GUARD
trampoline.label('skip')
trampoline.raw('eb32')                    # jmp HELD (0x1593cd0, past NC's 0x1593ca0..0x1593cc6)
trampoline.done()

# A colossus whose AI is not run (either slot) gets the host's handler at HELD_HOOK in its place
# (sdl_window.cpp CvcAiThunk: it keeps every register): motions the fight starts on it start there,
# where its AI would have started them, before the frame's animation update.
HELD, HELD_HOOK = 0x1593cd0, 0x1593d70
held = Stub(HELD)
held.raw('4885ff')                        # test rdi, rdi
held.short('74', 'out')
held.rip('488b05', HELD_HOOK)             # mov rax, [hook]
held.raw('4885c0')                        # test rax, rax
held.short('74', 'out')
held.raw('ffe0')                          # jmp rax (it returns to the AI's caller)
held.label('out')
held.raw('c3')
held.done()
assert trampoline.here() + 0x32 == HELD, hex(trampoline.here())
assert held.here() <= 0x1593d00  # MGR from there
assert held.here() <= HELD_HOOK, hex(held.here())

# The AI dereferences its colossus's workspace (0x746d50: type 9 -> object+0x50, then +0x1640)
# and 0x660fc0's character: a colossus being torn down (flag 0x10000 with its first word
# cleared) or one that stopped being a character (type no longer 9, or no workspace) mid-fight
# gets no AI (crashes at 0x82c0bf, 0x83a736).
guard = Stub(0x1593fd0)
guard.raw('f7470c00000100')               # test dword [rdi+0xc], 0x10000
guard.short('74', 'alive')
guard.raw('48833f00')                     # cmp qword [rdi], 0
guard.short('74', 'skip')
guard.label('alive')
guard.raw('807f0c09')                     # cmp byte [rdi+0xc], 9
guard.short('75', 'skip')
guard.raw('4883bf9016000000')             # cmp qword [rdi+0x1690], 0
guard.short('74', 'skip')
guard.raw('554889e54157')                 # 0x82c080 prologue
guard.jmp(0x82c086)
guard.label('skip')
guard.raw('c3')
guard.done()
assert guard.here() <= 0x1594000, hex(guard.here())

for stub, limit in ((query, 0x1593b00), (dual, 0x1593c80), (trampoline, 0x1593ca0), (held, HELD_HOOK)):
    ins = list(md.disasm(bytes(stub.code), stub.start))
    assert sum(i.size for i in ins) == len(stub.code), hex(stub.start)
    assert stub.here() <= limit, (hex(stub.start), hex(stub.here()))

if __name__ == '__main__':
    if '-v' in sys.argv:
        for stub in (query, dual):
            for i in md.disasm(bytes(stub.code), stub.start):
                print(f'{i.address:#x}: {i.mnemonic} {i.op_str}')
            print('--')
    patches = ((query.start, query.code), (dual.start, dual.code),
               (trampoline.start, trampoline.code), (held.start, held.code), (guard.start, guard.code),
               (0x82c080, jump_patch(0x82c080, dual.start, 6)))
    print('DUAL3=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in patches))


# 0x746d70 (an object's AI workspace, null-safe for a null object) dereferences 0x681ce0's
# answer, which is null for an object that is no longer a character (type 9): the opponent a
# colossus targets can turn into that mid-fight (0x746d80 crash). SAFEWS answers null then too.
safe = Stub(0x1593e90)
safe.raw('31c0')                          # xor eax, eax
safe.raw('4885ff')                        # test rdi, rdi
safe.short('74', 'out')
safe.raw('55')                            # push rbp
safe.raw('4889e5')                        # mov rbp, rsp
safe.rip('e8', 0x681ce0)                  # call 0x681ce0
safe.raw('4885c0')                        # test rax, rax
safe.short('74', 'pop')
safe.raw('488b8040160000')                # mov rax, [rax+0x1640]
safe.label('pop')
safe.raw('5d')                            # pop rbp
safe.label('out')
safe.raw('c3')                            # ret
safe.done()
assert safe.here() <= 0x1593ec0, hex(safe.here())
assert sum(i.size for i in md.disasm(bytes(safe.code), safe.start)) == len(safe.code)

if __name__ == '__main__':
    print('SAFEWS=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in
                               ((safe.start, safe.code), (0x746d70, jump_patch(0x746d70, safe.start, 5)))))


# 0x660e30(object, ...) binds a character to the object it stands on (the object's component:
# 0x660fc0's types, +0x120 / +0x40) and assumes one: for an object of another kind, or one being
# torn down, the component is 0 and it calls 0x5f6280 on 0 + 0xc0 (crash at 0x5f6280 reading
# 0xf0, seen once in ~50 CvC runs, called from 0x67dc1e). PARENTGUARD returns as the function's
# own "nothing to do" path does (0x660fa9) when there is no component.
# Each time it steps in, the u32 at GUARD_COUNT goes up (cvcbrawl logs it).
GUARD_COUNT = 0x1593b70
parent_guard = Stub(0x1593ac0)
parent_guard.raw('4d85ff')                  # test r15, r15
parent_guard.short('75', 'bound')
parent_guard.rip('ff05', GUARD_COUNT)       # inc dword [guard count]
parent_guard.jmp(0x660fa9)                  # xor eax, eax; epilogue
parent_guard.label('bound')
parent_guard.raw('4c89f7')                  # mov rdi, r14 (replaced)
parent_guard.rip('e8', 0x5f6280)            # call 0x5f6280 (replaced)
parent_guard.jmp(0x660e90)
parent_guard.done()
assert parent_guard.here() <= 0x1593b00, hex(parent_guard.here())
assert sum(i.size for i in md.disasm(bytes(parent_guard.code), parent_guard.start)) == len(parent_guard.code)

if __name__ == '__main__':
    print('PARENTGUARD=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in
                                    ((parent_guard.start, parent_guard.code),
                                     (0x660e88, jump_patch(0x660e88, parent_guard.start, 8)))))
