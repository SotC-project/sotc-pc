# Reporting problems and sending changes

## Before you open an issue

1. Read "If something goes wrong" at the end of [docs/TUTORIAL.md](docs/TUTORIAL.md) and the
   "Known limits" in the [README](README.md).
2. Search the open and closed issues for the same problem.
3. Try once more with the newest source of both repositories, built again.

## Opening an issue

Use the **Issues** tab and pick a form. One problem per issue. A good report has:

- **What you did**, step by step, and where in the game (title menu, which colossus, which
  area).
- **What you expected and what happened.** For a picture fault, a screenshot of the fault only.
- **Your PC**: graphics card, driver version, processor, memory, Windows version.
- **Which source**: the commit of this repository and of the runtime
  (`git rev-parse --short HEAD` in each folder).
- **The log** of the run that went wrong: `dist\user\log\shad_log.txt`. The file is written
  anew at each start, so copy it before you start the game again. Attach it as a file.
- Whether it happens every time, and since when.

The log holds the path of your game folder and of the program, which can include your Windows
user name. Look through it, and replace that name before you attach the file if you do not
want it public.

### Never attach or post

- Any file of the game: the program, assets, saves, texture or memory dumps, trophy data.
- PlayStation system software, modules, keys or firmware, or where to get any of these.
- Long recordings of the game. A short clip or a screenshot that shows the fault is fine.

Issues that contain such files or ask where to get the game are closed and the files removed.

### Kinds of report that help most

- **Other hardware.** Intel and NVIDIA cards are untested. A report that it works, with the
  card and driver, is as welcome as one that it does not.
- **Long frames.** A stop of a tenth of a second or more during play is treated as a fault.
  Start the game with the environment variable `SOTC_HITCH_MS=50` set; the log then names the
  cause of each long frame. Say where in the game it happened.
- **The game restarting by itself**, or closing with a message.
- **Something missing from the picture or drawn wrong.**

## Sending a change

- Changes to the scripts, settings and documents go to this repository; changes to the program
  itself go to the runtime repository, branch `sotc-pc`.
- Keep a pull request to one subject, and say how you tested it and on which hardware.
- For the runtime, follow the surrounding code's style; a change that affects every game
  rather than this one belongs upstream in shadPS4 first.
- Do not add game files, dumps, keys, or art taken from the game. Button pictures and the
  pointer are drawn by scripts in this repository for that reason.
- By sending a change you agree to license it under GPL-2.0-or-later, like the rest.

## Questions

Open an issue with the "Question" form. There is no other channel.
