# Guest Knight death: static findings

Analysis date: 2026-10-20. Static analysis of `img.bin` only; no game run, runtime edit, or commit. Addresses below are image offsets, not absolute pointers. Function names not present in strings are descriptive labels, not recovered symbols.

## Main finding

`0x4af970` is a **cutscene-event notification broadcaster**, not a cutscene/animation starter. A successful lookup and return from this function does not mean `KNIGHT_DEATH` was scheduled. The actual legacy cinematic starts through `0x83d960` and `0x6b1420`, using the `boss_param` entry's resolved `+0x1a0` descriptor. It is serviced later by `0x83dbc0`.

There are separate `KNIGHT_DEATH_MOT`, `KNIGHT_DEATH_SWITCH`, and `KNIGHT_DEAD_BODY_SWITCH` stage resources. The attempted extra pins `0xbf 0xc3 0xc7` are **player clothing switches**, not Knight death assets. The native stage activation path takes stage-table indices; the `0x3026330` array is indexed by those same indices when requested through `0x6af8e0`.

The strongest next experiment is to explicitly load/activate Knight death resources and start the Knight cinematic descriptor from a real guest script fiber, with loser-specific actor lookup/AI gating. Do not retry only `0x4af970("KNIGHT_DEATH")`.

## Verified native call chain

1. Knight script entry `0x6fe000`, string `void nico_KnightBossStart(void *)` at `0x127893b`, installs loop `0x6fe220` using `0x587cc0`.
2. Loop gets its character via `0x587cf0`; at `0x6fe23b` calls `0x6cb1d0(manager)`; at `0x6fe24b` calls `0x82c080(character)`; `0x6cc940` afterwards is just `ret`.
3. Health <= 0 at `0x6cc893` causes `0x82df90(character)` once (`manager+0x108` becomes 1), then `0x6da6e0(character)` each update. `0x6da6e0` only ORs 2 into `component+0xb0d`. It does not start an animation.
4. `0x82df90` stops/waits on AI tasks in aligned workspace+`0x4c40`. Its trailing call/jump path uses `0x746e00`, which obtains the **currently running fiber object**, then `0x746ee0`. Thus the supplied `rdi` is not enough to make this routine safe on an arbitrary runtime thread.
5. No dedicated Knight death branch exists in `0x6fe220`: the loop calls AI again after health update. A second externally driven AI update can also undo the intended stop. Explicitly stop dispatching `0x82c080` for the loser; setting health/death flags alone is not an AI exclusion predicate.

### Actual cinematic scheduler

`0x7e2eb0(bossParam*)` is the useful orchestration wrapper:

- `0x7e2edc`: reads `bossParam+0x1b0`, sets that global game flag via `0x7dbb00` if nonzero.
- `0x7e2f00..0x7e2f36`: resolves the typed sheet reference at `bossParam+0x1a0`, token at `+0x1a8`.
- `0x7e2f3c..0x7e2f43`: `0x83d960(resolvedDescriptor, 0, nullptr, &localHandle)`.
- Yields through `0x746e00` until a handle exists and `0x83d9f0(handle)` reports started.
- Registers `0x7e2fb0` via `0x8367f0`; callback eventually calls `0x83da10` to request stopping/fading the cinematic.
- Returns the cinematic handle.

`0x83d960` ABI: `rdi=descriptor`, `esi=flags`, `rdx=optionalPointer`, `rcx=handleOutputPointer`. Allocates one of three 0x48-byte global scheduler slots at `0x32935f0`; writes descriptor at slot+0x30, flags at +4, rdx at +0x38, output pointer at +0x40; sets state +8 to 1. Returns slot pointer or zero if all slots occupied. **The output pointer must remain alive until the scheduler consumes it.** Never pass a temporary host-stack address that expires on return.

`0x83dbc0` services these slots. At `0x83ddf0` it calls `0x6b1420(descriptor, flags&0xff, optionalPointer, handleOutputPointer)`. The returned cinematic index goes into slot+0; state becomes 2. `0x83dc70` eventually calls `0x6b17f0(index, floatTime)` when start requested. `0x83da00(handle,float)` writes slot+0xc and ORs bit 1 at +0x28 (start request); `0x83da10` requests stop.

`0x6b1420` reads descriptor+0xc as the ADPCM-sequence index, looks up 0x38-byte records from `[0x1654298+0x20]`, initializes a 0x788-byte cinematic context at `0x3236340 + index*0x788`, and starts resources. This is not an object-directed call: its inputs do not include the losing ISYSObj.

The global boss-death sequence at `0x7e1883` first calls `0x7d1890`, retains the **current level bossParam** in r13, and later calls `0x7e2eb0(r13)` at `0x7e1b86`. Guest health reaching zero does not make this r13 identify Knight. A current-boss hook scoped only to Knight's fiber will not affect this separate global orchestration fiber.

## Why the existing event call did nothing

`0x4af970` preserves state/name/cut/time, optionally prints them, then iterates `[this+0x10,this+0x18)` in 0x20-byte callback records. Each callback receives owner, state, name, cut, time. There is no name-to-cinematic lookup, queue allocation, animation load, or call to `0x83d960`/`0x6b1420` in it. `0x497320` (`cp10_cutsceneEvent`) merely forwards to this broadcaster after looking up the service. Hence the log `cutscene event KNIGHT_DEATH started` proves notification dispatch only.

Also, record `0x164dbf0` is not VM code. The descriptor name is `KNIGHT_DEATH`, and its typed references include `KNIGHT_SEQ` (`0x164d9d8`) of type `adpcmseq` (`0x1658ca8`). The sequence's entry `0x1659de0` packs count 1 and index `0xe6` in qword +8. That is a **sequence index**, not stage resource index `0xe6`.

## Knight descriptor and resources

Boss table: `[0x16d21f8+0x20]`; static image pointer value is `0x16d4010`; entry stride `0x1e8`.

Knight entry: `0x16d43e0` (`0x16d4010 + 2*0x1e8`). Relevant fields:

- `+0x28`: low dword 6, high dword 40. Do not infer both meanings without following consumers.
- `+0x88`: `0x12a`, matches Knight death stage switch index.
- `+0x168`: `0xc3`; `+0x16c`: `0xbf`; `+0x170`: `0xc7`. These are player clothing stage variants, not colossus motion assets.
- `+0x1a0`: reference `0x16d3e30`; this contains pointer to header `0x164d9d8` (`KNIGHT_SEQ`) and typed token `0x45c2603c44`.
- `+0x1a8`: token `0x54`; resolves to descriptor `0x164dbf0` using the standard sheet-reference resolver (12-bit index beginning at bit 18).
- `+0x1b0`: `0x1a1`.

Stage table: `[0x16d3848+0x20]`, static pointer `0x29a8cb0`, entry stride `0x30`. Entry +0 name, +8 reference, +0x10 backing sheet id, +0x2c stage ownership refcount.

| Stage index | Name | Backing sheet id |
| --- | --- | --- |
| 0xf8 | KNIGHT_BOOT_MOT | 0x672 |
| 0xf9 | KNIGHT_COMMON_MOT | 0x671 |
| 0xfa | KNIGHT_DEATH_MOT | 0x670 |
| 0x113 | KNIGHT_SWITCH | 0x657 |
| 0x12a | KNIGHT_DEATH_SWITCH | 0x640 |
| 0xbf | PLAYER_BODY_A_LV1_SWITCH | 0x6ab |
| 0xc3 | PLAYER_HEAD_A_LV1_SWITCH | 0x6a7 |
| 0xc7 | PLAYER_MANTLE_A_LV1_SWITCH | 0x6a3 |

Do not conflate stage ownership refcount (`stageEntry+0x2c`) with sheet manager request count (`[0x3026330]+stageIndex`). Incrementing an unloaded slot alone does not load assets, and can cause a subsequent real load to be skipped.

`0x7f81e0(partition, stageIndex, layoutIndex, bool)` loads a stage once and activates it:

- Increments stage entry+0x2c.
- If previous count was zero, queues `0x6af8e0(0x16d3848, stageIndex, &completion)` and pumps `0x6afa80(0)`.
- Waits with `0x746e00` for completion `0xdeaf`/`0xdeb0`.
- If newly loaded (`0xdeaf`), calls `0x747650(partition, stageIndex, layoutIndex, -1, bool, 0)` to instantiate layouts.

Typical native arguments: `rdi=image+0x2f35858` (partition object, **address**, not its first qword), `esi=stageIndex`, `edx=-1`, `ecx=0`.

`0x7f82c0(partition, stageIndex, layoutIndex, bool)` activates an already-loaded stage through `0x747650` with its final flag set to 1. It does not load missing motion data.

`KNIGHT_DEATH_MOT` contains animobj segment `KNIGHT_DEATH` at header `0x16ae150`; resolves to animobj definition `0x166a818` (type `animobj_def`, five entries starting `0x1696c20`):

1. `anim/knight_A_knight_death.anb`
2. `anim/knight_A_death_loop.anb`
3. `anim/objanim_knight_A_knight_death.anb`
4. `anim/camera_knight_death_camera_01.anb`
5. `anim/camera_knight_death_camera_02.anb`

`KNIGHT_DEATH_SWITCH` stage layout `0x29ec000` data `0x2a4b890` references:

- Gameobject header `0x16d3488`, name `KNIGHT_DEATH`; this is an alias-layout object whose data description is Japanese `騎士死亡エフェクト` (Knight death effects), not the living Knight character.
- Gameobject header `0x1b55800`, name `KNIGHT`; alias-layout definition ultimately references character header `0x1bbb9c0`, also name `KNIGHT`.
- Common data header `0x1bd8ed0`.

Thus merely spawning a `KNIGHT_DEATH` effect alias is not equivalent to collapsing the existing guest character. Stage activation can instantiate/rebind a Knight actor: check for duplicate characters and actor identity before using it as a final implementation.

There is a distinct model `nmo/knight_A_death.nmo` at resource `0x1a73b90`, model header `0x1a45188`; a distinct dead-body model also exists. This supports actor/model replacement during the full native death, rather than a universal "set death boolean" animation on any boss object.

## Concrete next experiment (not runtime-verified)

Use a queued callback on an existing **guest script fiber** with `0x587cf0()==loser`, or create a proper game script fiber. Do not call blocking/yielding functions directly from the SDL script thread or render callback.

1. At KO, exclude the loser from both native AI dispatch and dual-update trampoline. Keep the winner valid and keep the loser pointer stable while testing.
2. Before cinematic start, load/activate `0xfa` with `0x7f81e0(image+0x2f35858,0xfa,-1,0)`. Load/activate `0x12a` similarly only in an instrumented test because it can create another KNIGHT actor. Track the stage counts and balance releases later; do not just add bytes to unloaded refcounts.
3. Resolve the **Knight** bossParam from the table directly, not `0x7d1890` under Basaran's global orchestration. Call `0x7e2eb0(knightBossParam)` on that game fiber. This reproduces the actual descriptor scheduling and starts the proper KNIGHT sequence. It also sets flag `0x1a1` and installs stop handling, so snapshot/restore flags for isolated experiments.
4. If using the nonblocking scheduler directly instead: resolve `0x164dbf0` through the live Knight typed reference, call `0x83d960(descriptor,0,nullptr,persistentHandleStorage)`, wait for `0x83d9f0(handle)`, and call `0x83da00(handle,startTime)` once ready. Native `0x7e1b93` passes the float at `0x1289fb0`; read that exact value rather than assume 0. `0x7e2eb0` is preferable because it handles scheduling readiness and stop callback.
5. Instrument the character selected by the native cinematic actor binding. The scheduler does not accept `loser`; actor resolution is through descriptor/layout references. The two `KNIGHT` headers above identify where to inspect/rebind. If it spawns a fresh actor, copy/redirect its cinematic transform to the loser's KO transform and disable the living loser during cinematic, rather than allowing two visible Knights.

Step 3 has a statically proven native scheduling route but is **not** a proven clean guest collapse. Actor remapping and/or stage resources may still be needed. No claim that calling it alone solves CvC.

## Direct existing-object animation alternative

A concrete native motion setter exists at `0x635740`:

- `rdi=CharacterClass*` (`loser+0x120`), `rsi=resolved MotionKind*`, `xmm0=float transition/start parameter` (exact semantic not proven).
- Calls `0x635210(component,nullptr,motion,0,1,float)`, writes current motion pointer to component+0x260, initializes the animation through `0x611090(component+0x260,motion)`, and updates resolved motion branches via `0x635650`.
- Its object-facing caller is `0x6179f0`, which validates type 9/0x16, obtains component, resolves a motion handle using `0x65e690(component+0x13f8,...)`, sets root-motion state, then calls `0x635740`. The remaining arguments of `0x6179f0` were not fully recovered; do not guess its ABI.

**Do not pass `0x166a818` or `0x1696c20` to `0x635740`.** Those are AnimObjDef metadata, not MotionKind; the setter accesses motion+0x128, +0x100 and +0x110.

Candidate death motion names/data:

- `Knight_died_early` string `0x10eaf42`, record `0x1d39378`. Embedded AnimObjDef handle `0x1d36fe0` at `0x1d39498` resolves to `0x166a818`.
- `Knight_died_SA` string `0x1104e9f`, record `0x1f4d7f8`; handle `0x1f4a770` also resolves to `0x166a818`.

These links prove which animation the death motion uses, but **the correct MotionKind/Orient enclosing record and full setter ABI are not yet verified**. Use the working host death to capture component+0x260 and name, then reuse that resolved MotionKind on the guest only after ensuring `KNIGHT_DEATH_MOT` loaded. That is the shortest remaining runtime observation for an object-directed, spectator-camera-preserving death with no global cinematic.

## Limits

Static analysis identifies the scheduler, resource indices, separation of event notification from playback, guest/global context mismatch, and direct motion setter. It does not prove that the native cinematic automatically binds the retained guest object or that a particular MotionKind can safely be transplanted between differing model variants. Those require one instrumented host-vs-guest comparison, not speculative persistent patches.
