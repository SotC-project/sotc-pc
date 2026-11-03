# Colossus collision in the game's own PhysX (2026-10-21)

The eboot links PhysX (PxScene::addActors / addArticulation / fetchResults strings, pf.txt
6169-6182) plus physics::PhysicsManager. The line test 0x7a7e40 -> 0x84a260 -> 0x5aeeb0 returns,
for a colossus, its object at hit+0x20 and at hit+0x28 a word that looks like a PxRaycastHit face
index with flags in the top byte (logged: 0x4a7, 0x4a9, 0xc2/0x160000c2, 0xc5/0x960000c5,
0x1000012, 0x100000a, 0x2000015, 0x800047d6): the colossi carry triangle-mesh collision
(the climbing surfaces), hit per face.

Use for the rigid-body CvC (Jolt) plan: read those per-bone shapes/meshes and mirror them as
Jolt bodies instead of fitting capsules. Not yet located: where an object keeps its PxActors
and which bone each shape follows.

## Skeletons (from motion, re/cvc/skeleton.py, 900 samples each, 2026-10-21)

The component's bone matrices: 70 entries, 35-69 repeat 0-34 exactly, so 35 bones per colossus.
No parent table found by pointer scanning (only false trees); instead each bone's parent is
the bone it rides rigidly on (minimum spanning tree over offset spread, from bone 0), listed in
re/cvc/skeleton_parents.txt. Gaius: legs 13-14-15-16 and 17-18-19-20 (3.9, 5.0, 6.5), arms
1-2-4-3 and 5-6-8-7; sword bones 28, 31, 33 ride on nothing rigidly (spread 14-20): animated on
their own, so the sword needs its own body jointed to the hand. Basaran: legs 5-6-9-26,
4-7-8-25 (and 14-17-18-23, 16-19-24), head/neck through 11, 22, 27, 31. Zero-offset pairs are
helper bones at the same point.
Recorder: CVC_PROBE_SKEL=1 bash re/cvc_arena.sh 150 (testrun/pose_A.bin, pose_B.bin).

## Collision parts (line-test rays, re/cvc/collision.py, 2026-10-21)

48 rays a frame at each colossus (CVC_PROBE_SKEL, testrun/rays_*.bin). The hit word (+0x28) is
part << 24 | triangle (the top bit of the part byte set on about half: 0x80-0x96 next to
0x00-0x16, likely a second surface class). Every part's hits ride rigidly on one bone:
per-bone triangle-mesh collision. Gaius 30 parts; his sword is bone 32 (rigid on hand bone 20,
spread 0.02), parts 0x94 (blade, z 0-15.6) and 0x14 (tip, z 13-15). Basaran 36 parts; shell
0x00 on bone 30 (1501 triangles, box 15 x 13 x 16), head/neck 0x16 on 10, 0x96 on 31, legs
0x04-0x07/0x84-0x87 on 4-7, feet etc. The hit points of a part are a point cloud of its
surface in its bone's frame: convex hulls for the rigid-body plan.

## Basaran's "leash" (CVC_PROBE_LEASH, 2026-10-21)

Shoved along +x with MoveCharacter by 5, 10, 20, 40 units (10 s apart): it stays where put
(5.0, 15.0, 36.2-37.6 off its spot; no pull-back over 10 s each). At 77.6 off it left the
ground it stands on and fell 225 units. So there is no leash; the old snap-back after
Turtle_retreat_1 was the motion's own root motion, not a constraint. Bodies may be pushed
freely inside the arena; they must be kept on the ground (and in its bounds).
