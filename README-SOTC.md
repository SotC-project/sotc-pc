# Game-specific build of shadPS4 for Shadow of the Colossus (CUSA08809)

Unofficial. Not affiliated with, endorsed by or connected to Sony Interactive Entertainment,
Bluepoint Games or the shadPS4 project.

Branch `sotc-pc` is shadPS4 (see `README.md`, GPL-2.0-or-later, upstream commit 945dbc3) plus
changes made for one game: renderer and memory-tracking fixes, an audio synthesiser for the
game's sound engine, and an overlay with PC settings, key bindings, free play, an achievements
panel and extra lines in the game's own menus (`src/core/sotc/`).

It contains no game data. Run it with your own, legally obtained copy of the game. The scripts
that build, install and test it are in the companion wrapper repository.

Difference from the build on the author's PC: the program icon is shadPS4's.
