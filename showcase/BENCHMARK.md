# Shadow of the Colossus PC build: benchmark and requirements

Measured 2026-10-13 (first measured 2026-10-09) on the only PC available. Everything about other hardware is an estimate and
is marked as such.

## Test PC

| Part | Value |
|---|---|
| CPU | AMD Ryzen 7 5700X3D (8 cores, 16 threads) |
| GPU | AMD Radeon RX 9060 XT, 16 GB, driver Vulkan 1.4.349 |
| RAM | 32 GB |
| OS | Windows 11 |
| Build | optimised Release build of 2026-10-13 14:39 |

## Method

- Three scenes, started through free play: the third colossus's arena (Gaius), the fourth
  colossus's field with Agro, and the first colossus's arena (Valus).
- Scripted keyboard and mouse input walks and turns the camera for about 70 s; the last 60 s
  before the end of the run are measured. Frame rate uncapped, VSync off, 1920x1080 game image,
  hidden window.
- Average FPS is frames divided by time. "1% low" is the average frame rate over the slowest 1%
  of frames.
- The colossus moves on its own, so two runs of the same scene are not identical. Repeating the
  Medium preset moved its averages by up to 15 FPS. Read differences smaller than that as noise.
- The shrine is not included: reaching it needs menu steps the scripts do not do reliably.

## Results (average FPS / 1% low FPS)

Build of 2026-10-13, one run per cell:

| Preset | Gaius arena | Field with Agro | Valus arena |
|---|---|---|---|
| Low | 77 / 40 | 114 / 63 | 89 / 45 |
| Medium | 61 / 34 | 80 / 39 | 73 / 42 |
| High | 59 / 22 | 106 / 55 | 74 / 28 |
| Ultra | 59 / 35 | 102 / 56 | 81 / 38 |

Build of 2026-10-09, for comparison:

| Preset | Gaius arena | Field with Agro | Valus arena |
|---|---|---|---|
| Low | 91 / 38 | 121 / 68 | 105 / 71 |
| Medium (run 1) | 74 / 50 | 77 / 41 | 75 / 49 |
| Medium (run 2) | 68 / 48 | 92 / 66 | 79 / 48 |
| High | 60 / 31 | 91 / 60 | 83 / 56 |
| Ultra | 58 / 36 | 94 / 64 | 85 / 60 |

The newer build is not faster in these scenes: the differences between the two tables are
within the run-to-run spread described above. The work between them went into menus, input,
languages and start-up time.

What the presets change: Low turns off shadows, motion blur, depth of field and fur, with 2x
texture filtering and reduced draw distance. Medium turns shadows, depth of field and fur back on
with 4x filtering. High adds motion blur, 8x filtering and full draw distance. Ultra adds 16x
filtering and sharper textures. Mist particles are off in all of them (they render too bright in
this build).

Reading:
- Low is clearly faster than the rest (about 77-120 FPS against 58-106). Shadows, fur and depth
  of field are the options that cost.
- Medium, High and Ultra are within noise of each other on this PC. Filtering, draw distance
  and texture detail cost little here.
- The Gaius arena is the heaviest scene; its 1% lows are 22-50 FPS at every preset above Low.
- Stutters: 0 to 8 frames over 40 ms per 60 s. The worst single frames (135-204 ms) were in the
  Gaius arena and in the field.

Resource use of the game process during these runs:

| | Measured |
|---|---|
| RAM | 4.9 to 9.3 GB |
| Graphics memory | 3.8 to 8.2 GB (on a 16 GB card) |
| CPU | 2.7 to 3.8 cores busy of 16 threads |

## Upscalers: FSR 3.1, FSR 4, XeSS

Not available, and not benchmarked, because they cannot work here as things stand.

- All three are temporal upscalers. Each frame they need, besides the picture, the game's
  motion vectors, its depth buffer and the camera jitter. This runtime receives only the game's
  finished picture, after the game's own anti-aliasing and with the HUD already drawn in.
- The game does compute those inputs for its own anti-aliasing, so recovering them is not
  impossible in principle, but it means identifying those buffers inside the PS4 frame, which
  has not been done. Until then an "FSR 3.1" or "XeSS" entry would be a label with nothing
  behind it, so there is none.
- FSR 4 additionally comes as a driver upgrade of FSR 3.1 and needs the same inputs.
- What the build does have is FSR 1 (spatial upscaling plus sharpening) for windows larger than
  1080p. It needs none of those inputs. It was not part of this benchmark.
- A higher render resolution than 1080p is not available either: the game picks its own
  internal resolution.

## Requirements

Hard requirements (from the program itself, not estimates):
- 64-bit Windows 10 or 11.
- A CPU with AVX2 (the program is built for the x86-64-v3 level: Intel Haswell 2013 or newer,
  AMD Ryzen or newer). It will not start on older CPUs.
- A GPU and driver with Vulkan 1.3.
- The game's data from your own PS4 copy (about 14 GB) plus about 1 GB for caches and saves.

Recommended (measured: this is the test PC, 60 FPS or more on average at every preset):
- CPU: 8 cores / 16 threads of the Ryzen 7 5700X3D class.
- GPU: Radeon RX 9060 XT 16 GB.
- RAM: 32 GB installed (the game itself used up to 9.3 GB; 16 GB should be enough, not tested).

Minimum (estimate, not measured: no weaker PC was available):
- CPU: 6 cores / 12 threads with AVX2, for example Ryzen 5 3600 or Core i5-10400. The game kept
  about 2.7 cores busy on the test CPU; slower cores will lower the frame rate in proportion.
- GPU: 6 GB of graphics memory or more with Vulkan 1.3, for example GeForce GTX 1660 Super or
  Radeon RX 5600 XT, on the Low preset, aiming at 30 FPS. The game used 6.3-8.2 GB on a 16 GB
  card; how it behaves with less has not been seen.
- RAM: 16 GB.

Not tested at all: any Intel or NVIDIA GPU, any integrated GPU, any CPU other than the one
above. The Intel and NVIDIA lines in the estimates come from general performance class, not from
running the game on them.

## First-start settings

On a new player's first start the game picks a preset from the graphics memory it finds
(under 5 GB Low, 5-7 GB Medium, 7-11 GB High, 11 GB or more Ultra; never above Medium with less
than 15 GB of RAM or fewer than 8 CPU threads), sets the window to the display's size and turns
FSR 1 on for displays above 1080p. On the test PC it chose Ultra at 1080p. These thresholds are
a first guess from the single PC measured, not tuned on other hardware.
