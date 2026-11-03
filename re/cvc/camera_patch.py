"""Spectator camera: replace the gameplay camera's current pose after the game computes it.

0x7b8e64.. (rbx = camera+0x30) copies the camera's new pose into its current block at
camera+0x1740 (rbx+0x1710): position xyzw, look-at xyzw, unit direction xyz0, yaw (atan2 of the
direction's x, z), pitch, distance -- 15 floats -- then continues at 0x7b8fc2. CAMHOOK jumps there
to a stub that, while the u64 at 0x1593ee0 points at 15 floats (host-filled by cvccam=...:2), copies
them over the current block, then runs the replaced instruction (mov rdi, [rbp-0x148]).
Prints CAMHOOK=...
"""
import sys
from attack_patch import Stub, jump_patch, md

POSE_POINTER, AT, BACK = 0x1593ee0, 0x7b8fc2, 0x7b8fc9

# Line tests for the spectator camera: the game's own (0x7a7e40) with the camera collision's
# filter. Only the camera's tests pass r8b = 1 (0x7a2001, 0x7bd211, 0x7bd2af); CAPTURE, at that
# function's entry, keeps their filter (rcx) in FILTER. The pose stub first calls the host's
# handler at HOOK (it keeps every register), which runs the queued tests on this game thread
# (sdl_window.cpp CvcRayThunk).
FILTER, HOOK, LINE_TEST = 0x1593ed8, 0x1593ee8, 0x7a7e40
capture = Stub(0x1593dc8)
capture.raw('4584c0')                     # test r8b, r8b
capture.short('74', 'run')
capture.rip('48890d', FILTER)             # mov [filter], rcx
capture.label('run')
capture.raw('554889e54157')               # push rbp; mov rbp, rsp; push r15 (0x7a7e40 prologue)
capture.jmp(LINE_TEST + 6)
capture.done()
assert capture.here() <= 0x1593e00

stub = Stub(0x1593a70)
stub.rip('488b05', HOOK)                  # mov rax, [host handler]
stub.raw('4885c0')                        # test rax, rax
stub.short('74', 'pose')
stub.raw('ffd0')                          # call rax
stub.label('pose')
stub.rip('488b05', POSE_POINTER)          # mov rax, [pose pointer]
stub.raw('4885c0')                        # test rax, rax
stub.short('74', 'skip')
stub.raw('488dbb10170000')                # lea rdi, [rbx+0x1710] (camera+0x1740)
# The same 15-float pose is kept five times in a row (current, output, target, ... at camera
# +0x1740, +0x177c, +0x17b8, +0x17f4, +0x1830); rendering follows one of the later ones.
stub.raw('ba05000000')                    # mov edx, 5
stub.label('copy')
stub.raw('4889c6')                        # mov rsi, rax
stub.raw('b93c000000')                    # mov ecx, 60
stub.raw('f3a4')                          # rep movsb (rdi moves on to the next block)
stub.raw('ffca')                          # dec edx
stub.short('75', 'copy')
stub.label('skip')
stub.raw('488bbdb8feffff')                # mov rdi, [rbp-0x148] (replaced instruction)
stub.jmp(BACK)
stub.done()

ins = list(md.disasm(bytes(stub.code), stub.start))
assert sum(i.size for i in ins) == len(stub.code)
assert stub.here() <= 0x1593b00, hex(stub.here())

# The picture itself: 0x79dfd6 loads the render camera component and calls 0x590eb0, which builds
# the view (and everything after it: projection, culling) from its eye (+0x2b0), forward (+0x2c0)
# and up (+0x2d0) through 0x580280. RENDER writes the pose eye and direction there first.
RENDER_AT, RENDER_BACK, BUILD_VIEW = 0x79dfd6, 0x79dfde, 0x590eb0
RENDER_CAMERA = 0x1593d48  # where the stub leaves the render camera component for the host
render = Stub(0x1593f80)
render.raw('498b3e')                      # mov rdi, [r14] (replaced)
render.rip('48893d', RENDER_CAMERA)       # mov [render camera], rdi (the host draws in its view)
render.rip('488b05', POSE_POINTER)        # mov rax, [pose pointer]
render.raw('4885c0')                      # test rax, rax
render.short('74', 'build')
render.raw('c5f81000')                    # vmovups xmm0, [rax] (eye)
render.raw('c5f81187b0020000')            # vmovups [rdi+0x2b0], xmm0
render.raw('c5f8104020')                  # vmovups xmm0, [rax+0x20] (unit direction)
render.raw('c5f81187c0020000')            # vmovups [rdi+0x2c0], xmm0 (0x580280 takes a direction)
render.raw('c5f8104040')                  # vmovups xmm0, [rax+0x40] (up: no roll)
render.raw('c5f81187d0020000')            # vmovups [rdi+0x2d0], xmm0
# 0x590f01: with +0x370 set, 0x590eb0 turns the view further by the quaternion at +0x360 (the
# game's own aim offset); it would tilt this camera off the fight.
render.raw('c68770030000' + '00')         # mov byte [rdi+0x370], 0
render.label('build')
render.rip('e8', BUILD_VIEW)              # call 0x590eb0 (replaced)
render.jmp(RENDER_BACK)
render.done()
rins = list(md.disasm(bytes(render.code), render.start))
assert sum(i.size for i in rins) == len(render.code)
assert render.here() <= 0x1593fd0, hex(render.here())  # DUAL3 guard from there
ins += [None] + rins

if __name__ == '__main__':
    if '-v' in sys.argv:
        for i in ins:
            print('--' if i is None else f'{i.address:#x}: {i.mnemonic} {i.op_str}')
    print('CAMHOOK=' + ','.join(f'patch={a:x}:{bytes(c).hex()}' for a, c in
                                ((stub.start, stub.code), (AT, jump_patch(AT, stub.start, 7)),
                                 (capture.start, capture.code),
                                 (LINE_TEST, jump_patch(LINE_TEST, capture.start, 6)),
                                 (render.start, render.code),
                                 (RENDER_AT, jump_patch(RENDER_AT, render.start, 8)))))
