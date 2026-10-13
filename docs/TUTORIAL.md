# Tutorial: from source to playing

This page takes you from nothing to the title menu, then through the controls, the settings,
saving, Free Play and the achievements. It ends with what to do when something goes wrong.

No ready-made program is published. You build it once from source (steps 2 to 4); after that
you start the game from a desktop shortcut like any other.

**You need your own, legally obtained copy of the game, dumped from your own console.** This
project does not provide the game and cannot tell you how to get it.

## Getting it running

1. **Have the game on your PC.** You need the game's folder as dumped from your own PlayStation 4
   copy (about 14 GB; it holds `eboot.bin` and several `.psarc` files). Put it anywhere, for
   example `D:\Games\CUSA08809`. This project does not provide the game and cannot tell you how
   to get it.
2. **Install the build tools.** Visual Studio 2022 with the "Desktop development with C++"
   workload (it includes CMake), Git, and PowerShell 7.
3. **Get the source.** In a folder of your choice:
   ```
   git clone https://github.com/SotC-project/sotc-pc
   cd sotc-pc
   git submodule update --init runtime
   git -C runtime submodule update --init --recursive
   ```
   The build files expect the Clang compiler in `toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc`.
   Download that release from the LLVM project and unpack it there, or change the `PATH` line
   near the top of `build-lto.bat` to where your Clang is.
4. **Build.** Double-click `build-lto.bat`. The first build takes a while. It ends with
   `runtime\Build\x64-Clang-Release\shadps4.exe`.
5. **Install.** In PowerShell, from the `dist` folder:
   ```
   pwsh -File Install.ps1 -GameDir "D:\Games\CUSA08809"
   ```
   This copies the program into `dist` as `ShadowOfTheColossus.exe`, remembers where your game
   folder is, and puts a "Shadow of the Colossus" shortcut on the desktop and in the Start menu.
6. **Start the game** from the shortcut.

## The first start

7. Two short messages appear, this once only.
8. You are asked whether to **prepare shaders**. Choose **Prepare** (recommended): the game
   builds its graphics programs now, which takes a few minutes once, and then plays without the
   small stops that building them during play would cause. **Skip** starts at once and prepares
   them as you go; expect short stops the first time you see each place.
9. The game asks for your language (English (UK) is preselected), then shows a short settings
   page and a brightness screen. Confirm each with Enter. Careful with the arrow keys on the
   language list: Down moves to another language.
   The opening film plays; press Esc and choose Skip Cinematic to pass it, then any key at the
   title. The title menu appears: **Continue** and **Load Game** once you have a save, **New
   Game**, **Settings**, **Image Gallery**, **Free Play**, **Achievements**, **Quit**. Move with the arrow keys, the
   mouse or a controller; confirm with Enter, a click, or the bottom face button.

## Controls

A controller works as on the console; button pictures follow the device you last touched. These
are the default keys. Change any of them under Settings > Key Bindings.

| Action | Keyboard / mouse | Controller |
|---|---|---|
| Move | W A S D | Left stick |
| Look | Mouse | Right stick |
| Jump | Space | Cross / A |
| Attack; with the bow, draw (hold) and shoot (let go) | Left mouse button | Square / X |
| Grab, hold on, use, pick up, pray | E (hold), or the mouse's back side button | R2 / RT |
| Crouch, roll | C or Left Ctrl | Circle / B |
| Aim the bow, raise the sword to the light | Right or middle mouse button (hold) | R1 / RB |
| Call Agro; spur him on when riding | Q or Left Shift | Triangle / Y |
| Walk instead of run (on and off) | Left Shift | (tilt the stick less) |
| View the colossus | R (hold) | L2 / LT |
| Camera focus | Z | L1 / LB |
| Change sword / change ranged weapon | Mouse wheel, or 1 and 2 | D-pad right / left |
| Unequip | X | D-pad up |
| Photo mode | P | D-pad down |
| World map | Tab | Touchpad / Back |
| Zoom | F | R3 |
| Pause | Esc | Options / Start |

With the bow: hold the right mouse button to aim, hold the left to draw, let go to shoot. While
you aim, the mouse moves the aim.

The mouse does not turn the camera in menus or during scenes. In the game's menus the pointer
puts the cursor on the line it is over, a left click chooses, a right click goes back. The
pointer is hidden outside menus.

A scene can be left by holding Enter (or the controller's confirm button); a hint says so when
a scene begins.

The game has four button schemes of its own (Game Settings > Controls: Classic, Classic
Remapped, Modern, Modern Remapped). Under each of them the keys keep their actions: Space still
jumps, Q still calls Agro, the right mouse button still aims. The key pictures on screen and the
Controls page follow the scheme too. In the two Classic schemes crouching and rolling are on the
grab button, as the game has it there, so C and E do the same. The scheme is read when the game
starts; restart after changing it. A Free Play battle uses the same layout on a controller as
the story.

The game runs at the same speed at any frame rate.

## Settings

10. Choose **Settings** on the title menu or in the pause menu. It offers **Graphics**,
    **Key Bindings** and **Game Settings** (the game's own display, audio, camera and controls).
11. The first start picks graphics settings from your graphics card and screen. To change them,
    open **Graphics** and choose a **Preset**: Medium is a good start; try High if the game runs
    well, Low if it is slow (Low is clearly the fastest). You can also set the frame rate limit
    (30 to 240, or uncapped), VSync, full screen, window size, FSR upscaling and each effect by
    itself. Then choose **Apply and Restart**; the game restarts by itself.
12. If the camera moves the wrong way for you, change it under Game Settings > Camera.

The company logos at the start are left out. **Startup Logos** under Graphics brings them back.

## Language

The game's own language is set under Game Settings > Language (Text and Subtitles). What this
project adds (the extra menu lines, the Settings pages, Free Play, Time Attack, Achievements, the
save list, the hints) follows the Text language after a restart of the game.

Tables exist for all the game's languages besides English: Arabic, Danish, Dutch, Finnish,
French, German, Italian, Norwegian, Polish, Portuguese, Russian, Spanish, Swedish and Turkish,
and for Hebrew, which the game itself does not have. They are machine translations that no
native reader has checked yet; corrections are welcome. Each is a plain text file in `lang`
(English text, a tab, the translation; `keys.txt` is the English list), so you can correct a
line and restart.

Arabic and Hebrew are written from right to left with their letters joined; with the game in
Arabic the added menu lines stand on the right as the game's own do. The Settings and Free Play
pages keep their left-hand layout in those languages.

To use a table the game has no language for, write its name (for Hebrew: `he`) into a file
`sotc_language.txt` in the `user` folder. The game's own texts then stay in the game's language.

Not translated: the names of keys (Enter, Left Shift, Right mouse). The achievement names are
our own translations, not the official ones.

## Saving

13. The game saves by itself as you play. **Manual Save** in the pause menu opens a list of
    your saves: choose **New Save Game**, or an existing save to write over it (the game asks
    first, with No preselected), with the arrow keys and Enter or the mouse. Esc leaves the
    list. **Load Game** on the title menu shows the same list; one press of Enter loads the
    save.
    Saves live in `dist\user`; keep that folder if you move or rebuild the program.
14. **Main Menu** in the pause menu goes back to the title menu. **Quit** on the title menu
    closes the game.

## Free Play and Time Attack

15. Choose **Free Play** on the title menu. Pick a **Colossus** and a **Difficulty** with left
    and right. The other lines (all weapons, invincibility, unlimited grip, maximum damage) are
    optional helpers.
16. Choose **Begin Battle**. The game restarts and loads that battle; this takes some seconds.
17. For a graded fight, turn **Time Attack** on first. The helpers are then off. A small clock
    appears top left and starts when you first move. When the colossus falls, or you do, a
    result page shows your time, the target time, the health you lost and a rank from D to S+.
18. During a free play battle the pause menu (Esc) is the game's own: **Settings** as in the
    story, and **Main Menu** to leave the battle (the game restarts for it).

## Achievements

19. **Achievements** on the title menu lists all 38 with your progress. They unlock as the game
    awards its trophies, with a small notice on screen. Nothing unlocks in Free Play.

## If something goes wrong

- **The game stops and starts again by itself.** If the game's own program faults, the port
  starts the game again and says so on screen. Your progress up to the last save is kept. A
  fault that used to happen about once in nine returns to the title menu has been fixed; if
  you see a restart, please report it with the log (see below).
- **Short stops when entering a new area.** Choosing Prepare at the first start removes most of
  them. While a battle loads, the loading animation can hold still for about half a second,
  once, before the battle appears.
- **Something missing from the picture in one place** (a creature or object not drawn). The port
  limits four oversized shader buffers to 64 MB each; nothing has been seen to need more. To
  test without the limit, set the environment variable `SOTC_UNBOUNDED_CAP_MB=0` before
  starting the game, and report it if that brings the object back.
- **A message that the graphics driver stopped.** Seen with a screen recorder running on an AMD
  card. Close the recorder and start the game again.
- **Intel and NVIDIA graphics cards have not been tested.** It may work; nobody has checked.
- **To report a problem**, see [CONTRIBUTING.md](../CONTRIBUTING.md): it says what to attach
  and what never to attach.
