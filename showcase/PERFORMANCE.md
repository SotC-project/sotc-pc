# Shadow of the Colossus PC build: where the frame time goes (multithreading request)

Status: measured and analysed. No threading or optimisation change has been made yet from this
analysis, so there are no before/after numbers. This page says what limits the frame rate, what
can and cannot be spread over more cores, and what would be worth doing, with the expected gain.

Measured 2026-10-09 in the heaviest benchmark scene (third colossus arena), optimised build,
Ryzen 7 5700X3D + Radeon RX 9060 XT, about 73-80 FPS at the time.

## CPU use by thread

| Thread | Share of one core |
|---|---|
| GPU command translation (`GpuCommandProcessor`) | 92-97% |
| Game job workers (4 threads) | 25-41% each |
| Timers (input polling) | 12-15% |
| Game update thread | 6-11% |
| Audio | 5-9% |
| Game main thread | 4-8% |
| Present thread | 2-3% |

Total about 2.6-2.9 cores of 16 threads. One thread is saturated and everything else has room:
the frame rate is capped by that one thread, not by the GPU and not by the game's own code.

## What that thread does

It reads the PS4 graphics command stream the game produces and turns it into Vulkan calls. Per
frame in this scene that is about 2,000 draws and 1,000 compute dispatches. A sampling profile
of the thread (10,000 samples over 20 s):

| Work | Share of the thread |
|---|---|
| Draws (`DrawIndirect` 42%, `Draw` 13%) | 55% |
| of which binding buffers and textures for each draw | 28% |
| Compute dispatches | 25% |
| Time inside the AMD Vulkan driver, all calls together | 23% |
| Looking up vertex/constant buffers (`ObtainBuffer`) | 13% |
| Looking up textures (`FindImage`, `BindTextures`) | 11% |
| Finding the pipeline for each draw (shader specialisation and compare) | 8% |
| Copying compute results back to game memory (includes waiting for the GPU) | 7% |
| Memory page protection changes for change tracking | 4% |
| Re-creating one image twice per frame (see below) | about 5% |

## Why this is hard to spread over threads

- The command stream is a state machine. Each command changes state the next one depends on
  (bound shaders, buffers, render targets), and the game's memory changes between commands. It
  has to be read in order.
- The caches the thread consults (textures, buffers, pipelines) are not safe to use from several
  threads, and they also track which game memory the GPU has written, in order.
- The renderer records Vulkan commands directly as it reads. Moving the recording to a second
  thread would take the driver's 23% off the limiting thread, which is the single biggest
  realistic win from threading (roughly +25% frame rate if nothing else moved). It means
  changing every place that records a command, across the whole renderer. That is a large,
  risky change and is not done.
- What cannot be threaded at all: the order of commands, change tracking of game memory, and
  the waits for GPU results that the game reads back on the CPU.

## Work that could be cut instead (not done yet)

| Item | What happens now | Expected gain | Risk |
|---|---|---|---|
| Depth image re-created every frame | An 800x450 depth buffer is used as a depth target and then as a plain float image by a compute pass. Each switch destroys the image and creates the other kind, with a copy: about 120 image creations per second. Keeping both versions alive and only copying would remove the creation cost. | about 5% | medium: wrong handling shows as broken shadows or ambient occlusion |
| Pipeline lookup per draw | The shader specialisation key is rebuilt and compared for every draw even when shaders and bound resource types did not change. Skipping when nothing changed. | about 5% | medium: a wrong skip draws with the wrong shader variant |
| Texture lookup | Linear work per texture bind; a last-result cache per binding slot. | 3-5% | low to medium |
| Record Vulkan commands on a worker thread | See above. | up to 25% | high, large change |

These add up to perhaps 10-15% from the first three, and more from the fourth. They are
estimates from the profile, not measurements.

## Already in place from earlier work

Repeated buffer and texture binds are skipped (about 120,000-160,000 skipped per 3 s in this
scene), compute outputs are copied back only when the game needs them, and saved pipelines are
loaded at start instead of being compiled during play. Those took the game from about 51 to
about 64-80 FPS in the test scenes.

## Update: step 1, the cheaper cuts (measured)

Two cuts are in the installed build. Measured back to back, same build, cuts switched off
(baseline) and on, two runs each, High-like settings of the test profile, 60 s per run.
Average FPS / 1% low FPS:

| Scene | Baseline run 1 | Baseline run 2 | With cuts run 1 | With cuts run 2 |
|---|---|---|---|---|
| Gaius arena | 69 / 49 | 72 / 48 | 70 / 50 | 72 / 52 |
| Valus arena | 79 / 46 | 80 / 50 | 86 / 57 | 87 / 59 |

- Valus arena: about +8% average and +10 FPS on the 1% low.
- Gaius arena: no measurable change in the average (within noise), 1% low up by 2-3 FPS.
- Screenshots with and without the cuts show the same picture.

What the two cuts are:
1. The 800x450 depth image that was destroyed and re-created about 120 times per second is now
   kept in both of its forms and only copied. Image creations fell to a handful per second.
2. Decoding a texture descriptor into the image description is cached per descriptor (it was
   recomputed for every texture bind of every draw). This stands in for the "texture lookup
   cache" item: it caches only a pure calculation, so it cannot return a stale texture.

Not done, and why:
- Skipping the per-draw shader variant rebuild. The variant depends on resource descriptors
  that live in game memory and can change between any two draws, so they must be re-read each
  time; a skip that guesses wrong draws with the wrong shader. The saving at stake is about 4%
  of the thread. Judged not worth the risk.

Correction to the table above: a second profile that charges driver time to the calling
function shows about 11-15% of the limiting thread inside the Vulkan driver, not 23%. Recording
commands on a worker thread would therefore be worth roughly +12-15% frame rate, not +25%.
The rest of the thread's time is in lookups and change tracking that cannot move.

Step 2 (worker-thread command recording) was scoped and then not built; see below.

## Update: step 2, worker-thread command recording (scoped, not built)

I surveyed the change and sized the gain from the profile before writing it. Result: the
ceiling is about +7% frame rate, for the riskiest change in the renderer. I have not built it.

How it would work: the renderer records about 56 different Vulkan commands at about 150 places.
The commands issued for every draw (bind pipeline, push descriptors, push constants, bind vertex
and index buffers, dynamic state, draw, dispatch) would be written to a queue and replayed into
the real command buffer by a second thread. Everything else (barriers, copies, render pass
begin and end, submits) would first wait for that thread to catch up, then run directly, so the
order of commands is kept.

Why the gain is small:
- The profile charges these shares of the limiting thread to driver calls made per draw:
  draw calls 5.7%, descriptor and pipeline binds 3.3%, dispatches 0.7%. That is 9.7% that
  could move.
- Queueing is not free. Descriptor writes point at arrays that are reused for the next draw,
  so each one has to be copied into the queue: an estimated 2-3% added back.
- Every command that is not queued forces a wait for the second thread. The inFAMOUS port,
  which has the same design, tried a worker for upload copies and measured it slower for
  exactly this reason.
- Net: about 6-7% of the thread, so about +7% frame rate at best, and less if the waits bite.

Why the risk is high: a command replayed out of order, or a queued pointer that outlived its
data, shows up as corrupted rendering or a device loss, in any scene, and only sometimes.

Recommendation: do not build this now. The same effort is better spent on what still breaks
play (the rare crashes, the mist particles). If it is still wanted after that, the design above
is where to start, on its own branch as agreed.

## Update: step 2 built and measured

Built after all, on the branch `worker-recording`, at the user's request. It works and gives a
modest gain in the heaviest scene.

What was built: the commands issued for every draw (pipeline and vertex buffer binds,
descriptor and constant pushes, dynamic state, draws, dispatches, render pass begin and end, and
the barrier flush before a draw) are written to a queue by the GPU command thread and replayed,
in order, by a second thread. Any other command first waits for that thread to catch up and is
then recorded directly, so the order never changes. Upload command buffers got their own command
pool, because a pool may only be used by one thread at a time. `SOTC_WORKER_RECORD=0` switches
back to recording in place.

Measured on the same optimised build, worker off and on, two runs each (average / 1% low FPS):

| Scene | Worker off, run 1 | Worker off, run 2 | Worker on, run 1 | Worker on, run 2 |
|---|---|---|---|---|
| Gaius arena | 70 / 49 | 70 / 47 | 75 / 52 | 74 / 51 |
| Valus arena | 80 / 49 | 86 / 58 | 80 / 53 | 89 / 62 |

- Gaius arena: about +6% average, +3 FPS on the 1% low.
- Valus arena: inside the run-to-run spread (the two baseline runs differ by 6 FPS); no claim.
- CPU: the GPU command thread fell from 88-94% to 81-84% of a core; the new recorder thread
  uses 73-79% (part of that is short busy-waiting for work, by design).
- The command thread is no longer pinned at its limit, so what holds the frame rate now is
  mostly the waits for results the game reads back from the GPU, which threading cannot remove.

Render checks with the worker on, all clean: Gaius arena, Valus arena, the fourth colossus's
field with Agro, the fifth colossus's arena, and the overlay screens (menu, achievement popup,
results). Nine runs in total, no crash, no corruption seen. This is scripted testing, not a play
session.

It matches the estimate (about +7% at best) and is now in the installed build.

