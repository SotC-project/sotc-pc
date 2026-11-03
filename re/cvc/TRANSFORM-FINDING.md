# CvC transform investigation — 2026-10-19

## Superseding live evidence

The type-0 correction below does NOT apply to Gaius. A filtered hook at `0x6611c0` matched the component's position pointer to Gaius+0x130 and captured component = Gaius+0x120. Its transform variant at component+0xf0 was **2**, not 0. Type 2 calls `0x59a8d0` with translation at transform+0x18, hence actor+0x1f8. That translation is parent/bone-relative, not a world-space delta.

Observed parent: `0x361c148d0`, handle `0xffffffff80000006`; addresses vary per run. `0x59a8d0` resolves parent via `0x660fc0`, reads matrices at parent-component+0x1a0, selects byte offset `(handle >> 0x12) & 0x1fc0`, and multiplies the four local coordinates by that matrix. Reliable world-space relocation requires the actual matrix and its inverse, or an existing native world-position setter.

The live bytes at `0x660fc0`, `0x6611c0`, and `0x611510` matched the cached image exactly. No version mismatch. Removed guessed local-field writes from teleport, floor and synthetic push in source; world-copy-only writes remain diagnostic, not a persistent relocation fix. The old `check_translation.py` only proves type-0 layout and must NOT be used as validation of Gaius relocation.

## Earlier hypothesis (retained as failed investigation)


The previous placement recipe wrote the local translation at object+0x1f8. Disassembly establishes object+0x1f0 instead:

- `0x611510` returns character block `object+0x120`.
- `0x66120c` passes `character+0xc0` to `0x5f62b0`.
- Transform type 0 (`transform+0x30 == 0`) copies translation from `transform+0x10/+0x14/+0x18` at `0x5f6360..0x5f6378`.
- Therefore XYZ is `object+0x1f0/+0x1f4/+0x1f8`; W is `+0x1fc`.

The old teleport missed local X and changed local Z, W, and the next word. The old floor clamped W rather than Y. Also removed writes to guessed copies `+0x1410/+0x14d0`: dumps show `+0x1418` holds a pointer, not a third position component.

Checks: `python re/cvc/check_translation.py`, `python re/cvc/check_patches.py`. These establish instruction/layout consistency, not successful gameplay.

Latest mutual-target run had two live colossus objects but Gaius remained near `(288,46,-1389)`, far from Basaran near `(844,18,-1061)`. The captured frame showed only Basaran. Two following one-way baseline attempts were invalid: both target slots were zero when configured. Do not interpret normal process exit or stale dumps as a successful fight.

After rebuilding with the correction, two valid mutual-target runs completed without a reported crash. In the longer run, planar separation decreased from 391.96 to 118.88 units (snapshots 17 through 25); Gaius moved from `(662.59,58.73,-1415.54)` to `(771.76,59.12,-1199.98)`, while Basaran ended at `(817.12,15.97,-1090.09)`. This proves sustained approach, not attack contact or damage. A longer close-range run then failed to reproduce placement: Gaius stayed at `(288.19,46.46,-1388.68)` throughout while Basaran moved away. Thus approach is not yet reliable. Inspection shows the local field is a transient delta, consumed only when character+0x140 (object+0x260) is set; `0x661b2e` tests it and `0x661b5f` clears it after applying the transform. Explicitly setting object+0x260 after placement crashed at eboot+0x5fbfcc (list unlink). The pre-write word there was float data (59.298832), not a verified flag. Do not repeat that write. The assumed component base remains unverified against live instructions; a read-only live-code dump is pending. The preceding write-watch run was invalid because both target slots were zero.

`re/cvc_mutual_probe.sh [mutual|oneway]` runs without synthetic damage and validates nonzero target objects afterward. Runtime source corrections are not installed in the user's game.

Native defeat analysis: manager health depletion at `0x6cba55` sets character-component `+0x240` to 1 (`0x6cba7f`), then performs global boss identity/win-stat handling through `GetCurrentBoss` and table `0x16d5e90`. This is a story-global flow, not an independent per-colossus match result; do not call it a solved defeat sequence.
