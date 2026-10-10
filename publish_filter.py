"""Tree filter for the published runtime branch (run by git filter-branch in the work tree of
each commit): removes the program icon that was drawn after a third party's artwork, puts the
upstream icon back wherever it was replaced. (The commits up to 2026-10-10 were also given
placeholder trophy names; the names are published from the next commit on.)"""
import os
import re

for path in ('src/dist/sotc.ico', 'src/resources/sotc_icon.png'):
    if os.path.exists(path):
        os.remove(path)


def rewrite(path, change):
    if not os.path.exists(path):
        return
    with open(path, encoding='utf-8', newline='') as f:
        old = f.read()
    new = change(old)
    if new != old:
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(new)


rewrite('src/dist/shadps4.rc', lambda s: s.replace('ICON "sotc.ico"', 'ICON "shadps4.ico"'))
rewrite('CMakeLists.txt', lambda s: re.sub(r'[ \t]*src/resources/sotc_icon\.png\r?\n', '', s))
rewrite('src/sdl_window.cpp', lambda s: re.sub(
    r'[ \t]*// This build carries its own icon for the window and taskbar\.\r?\n[ \t]*\{\r?\n'
    r'(?:.*\r?\n)*?[ \t]*return;\r?\n[ \t]*\}\r?\n', '', s, count=1))
