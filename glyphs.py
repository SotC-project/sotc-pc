"""Builds keyboard/mouse prompt textures that replace the game's controller button glyphs.

Each output is <hash of the original texture>.bin: an RGBA8 mip chain the runtime loads from
user/sotc_textures (see TextureCache::ApplyTexturePack). Run: python glyphs.py <output dir>
"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

# Hash of the original 128x128 glyph -> label of the key bound to that button in CUSA08809.ini.
GLYPHS = {
    '25b54a4ac0e4e967': 'Shift',  # triangle: control horse
    'dbb1926987bdbf8e': 'Space',  # cross: jump
    'f87d7fa10c4d64d9': 'C',      # circle: roll
    '8359520e683c7c4c': 'R',      # L2: view colossus
    'c0b33fa3aace5af6': 'P',      # d-pad down: photo mode
    '6ff0fae0c49fb077': '2',      # d-pad left: change ranged weapon
    '202c5bb2cb1587f9': '1',      # d-pad right: change sword
    'cc10ab8a4ccb5c32': 'LMB',    # square: attack
    'cd10bb02a59e9d9d': 'RMB',    # R2: grab
    '59acf8ac22bce6e6': 'WASD',   # left stick: move
    '957207112e25fe2c': 'Q',      # R1: aim, raise the sword
    '8ed179fdd7069c2a': 'Mouse',  # right stick: camera
    '833e70d22ee964a8': 'Z',      # L1: camera focus, previous page
    '0c0501623942a092': 'V',      # L3
}
# Xbox pad: face buttons by letter and colour, triggers by name. The d-pad glyphs fit as they are.
XBOX = {
    'dbb1926987bdbf8e': ('A', (120, 200, 70)),
    'f87d7fa10c4d64d9': ('B', (225, 70, 60)),
    '25b54a4ac0e4e967': ('Y', (240, 200, 50)),
    '8359520e683c7c4c': ('LT', (235, 235, 235)),
    'cc10ab8a4ccb5c32': ('X', (70, 140, 235)),
    'cd10bb02a59e9d9d': ('RT', (235, 235, 235)),
    '957207112e25fe2c': ('RB', (235, 235, 235)),
    '833e70d22ee964a8': ('LB', (235, 235, 235)),
    '0c0501623942a092': ('LS', (235, 235, 235)),
}
SIZE = 128


def keycap(label):
    scale = 4  # draw large, downsample for smooth edges
    big = SIZE * scale
    image = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    wide = len(label) > 1
    inset_x, inset_y = (4 if wide else 18) * scale, (30 if wide else 18) * scale
    box = (inset_x, inset_y, big - inset_x, big - inset_y)
    draw.rounded_rectangle(box, radius=14 * scale, fill=(28, 28, 30, 235),
                           outline=(225, 225, 225, 255), width=4 * scale)
    font_name = 'seguisym.ttf' if not label.isascii() else 'arialbd.ttf'
    # Five letters (Mouse, Space, Shift) only fit the cap at a smaller size.
    font = ImageFont.truetype(font_name, ((40 if len(label) <= 4 else 32) if wide else 60) * scale)
    draw.text((big / 2, big / 2), label, font=font, fill=(255, 255, 255, 255), anchor='mm')
    return image.resize((SIZE, SIZE), Image.LANCZOS)


# The Controls screen: an 812x524 controller drawing with leader lines to eight label slots on
# each side, which the game captions with the action names. The replacement keeps the slots and
# names the bound key or mouse button next to each.
DIAGRAM_HASH = '4cc0bdb755626df7'
# The same drawing at twice the size, in the menus of a normal start.
DIAGRAM_LARGE_HASH = 'a385b4851288a6a9'
DIAGRAM_ROWS = (44, 102, 160, 218, 277, 335, 393, 451)
DIAGRAM_LEFT = ('Tab', 'R', 'Z', 'X  /  Up', 'Wheel up  /  2', 'P  /  Down',
                'Wheel down  /  1', 'W  A  S  D')
DIAGRAM_RIGHT = ('Esc', 'Right mouse  /  E', 'Q  /  Middle mouse', 'Left Shift',
                 'C  /  Left Ctrl', 'Space  /  Enter', 'Left mouse button', 'Mouse')


def diagram(large=False):
    scale = 2
    image = Image.new('RGBA', (812 * scale, 524 * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype('arialbd.ttf', 20 * scale)
    ink = (0, 0, 0, 255)
    for y, left, right in zip(DIAGRAM_ROWS, DIAGRAM_LEFT, DIAGRAM_RIGHT):
        y *= scale
        draw.line((42 * scale, y, 90 * scale, y), fill=ink, width=2 * scale)
        draw.text((102 * scale, y), left, font=font, fill=ink, anchor='lm')
        draw.line((722 * scale, y, 770 * scale, y), fill=ink, width=2 * scale)
        draw.text((710 * scale, y), right, font=font, fill=ink, anchor='rm')
    return image if large else image.resize((812, 524), Image.LANCZOS)


def pad_button(label, colour):
    scale = 4
    big = SIZE * scale
    image = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    inset = 14 * scale
    draw.ellipse((inset, inset, big - inset, big - inset), fill=(24, 24, 26, 240),
                 outline=(215, 215, 215, 255), width=4 * scale)
    font = ImageFont.truetype('arialbd.ttf', (44 if len(label) > 1 else 64) * scale)
    draw.text((big / 2, big / 2), label, font=font, fill=colour + (255,), anchor='mm')
    return image.resize((SIZE, SIZE), Image.LANCZOS)


def write(out, name, image):
    data = b''
    size = SIZE
    while size >= 1:
        data += image.resize((size, size), Image.BOX).tobytes()
        size //= 2
    (out / f'{name}.bin').write_bytes(data)
    image.save(out / f'{name}.png')  # preview only; the runtime ignores it


# Pad output in the bindings file -> glyph texture it is drawn with.
BUTTON_GLYPHS = {
    'triangle': '25b54a4ac0e4e967', 'cross': 'dbb1926987bdbf8e', 'circle': 'f87d7fa10c4d64d9',
    'l2': '8359520e683c7c4c', 'pad_down': 'c0b33fa3aace5af6', 'pad_left': '6ff0fae0c49fb077',
    'pad_right': '202c5bb2cb1587f9', 'square': 'cc10ab8a4ccb5c32', 'r2': 'cd10bb02a59e9d9d',
    'r1': '957207112e25fe2c', 'l1': '833e70d22ee964a8', 'l3': '0c0501623942a092',
}
STICK_GLYPH = '59acf8ac22bce6e6'
MOVE_OUTPUTS = ('axis_left_y_minus', 'axis_left_x_minus', 'axis_left_y_plus', 'axis_left_x_plus')
# Names of keys on a keycap (short) and in the Controls diagram (long).
SHORT = {'sidebuttonback': 'M4', 'sidebuttonforward': 'M5', 'leftbutton': 'LMB', 'rightbutton': 'RMB', 'middlebutton': 'MMB', 'mousewheelup': 'Wh+',
         'mousewheeldown': 'Wh-', 'lshift': 'Shift', 'rshift': 'Shift', 'lctrl': 'Ctrl',
         'rctrl': 'Ctrl', 'lalt': 'Alt', 'ralt': 'Alt', 'space': 'Space', 'enter': 'Enter',
         'escape': 'Esc', 'tab': 'Tab', 'backspace': 'Bksp', 'up': 'Up', 'down': 'Down',
         'left': 'Left', 'right': 'Right'}
LONG = {'sidebuttonback': 'Mouse back', 'sidebuttonforward': 'Mouse forward',
        'leftbutton': 'Left mouse button', 'rightbutton': 'Right mouse', 'middlebutton': 'Middle mouse',
        'mousewheelup': 'Wheel up', 'mousewheeldown': 'Wheel down', 'lshift': 'Left Shift',
        'rshift': 'Right Shift', 'lctrl': 'Left Ctrl', 'rctrl': 'Right Ctrl', 'lalt': 'Left Alt',
        'space': 'Space', 'enter': 'Enter', 'escape': 'Esc', 'tab': 'Tab', 'backspace': 'Backspace',
        'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right'}
PAD_NAMES = {'triangle', 'cross', 'circle', 'square', 'l1', 'l2', 'l3', 'r1', 'r2', 'r3', 'options',
             'back', 'pad_up', 'pad_down', 'pad_left', 'pad_right', 'axis_left_x', 'axis_left_y',
             'axis_right_x', 'axis_right_y', 'touchpad_center', 'touchpad_left', 'touchpad_right'}


def short_name(key):
    return SHORT.get(key, key.upper() if len(key) <= 3 else key.capitalize())


def long_name(key):
    return LONG.get(key, key.upper() if len(key) <= 3 else key.capitalize())


# The bindings file is written for the game's default button scheme (Modern). In the story under
# another scheme the runtime moves each key to the button that has the same action there (see
# ButtonSchemes in the runtime's input_handler.cpp; keep the two alike): Enter and Backspace stay
# where they are for the menus, and in the classic schemes the crouch key goes to the grab button.
# Scheme name in the game's profile -> (button in the default scheme -> button there, roll button).
SCHEMES = {
    'Legacy': ({'cross': 'triangle', 'triangle': 'cross', 'r2': 'r1', 'r1': 'circle',
                'l2': 'l1', 'l1': 'l2', 'r3': 'r2'}, 'r1'),
    'LegacyRemap': ({'r1': 'circle', 'r3': 'r1'}, 'r2'),
    'ModernRemap': ({'cross': 'triangle', 'triangle': 'cross', 'r2': 'r1', 'r1': 'r2',
                     'l2': 'l1', 'l1': 'l2'}, ''),
}
MENU_KEYS = ('enter', 'backspace')


def read_bindings(path, scheme=None):
    """Keyboard and mouse inputs per pad button, in file order; the menu keys come last, so a
    button is pictured by the key that does its action in play."""
    bound, menu = {}, {}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.split('#')[0]
        if '=' not in line:
            continue
        output, key = (part.strip() for part in line.split('=', 1))
        if output == 'square_or_bow':  # attack key that also draws the bow while it is out
            output = 'square'
        if not output or not key or ',' in key or key in PAD_NAMES:
            continue
        if key in MENU_KEYS:
            menu.setdefault(output, []).append(key)
            continue
        if scheme:
            moved, roll = SCHEMES[scheme]
            output = roll if output == 'circle' and roll else moved.get(output, output)
        bound.setdefault(output, []).append(key)
    for output, keys in menu.items():
        bound.setdefault(output, []).extend(keys)
    return bound


def apply_bindings(path, scheme=None):
    """Replaces the default labels by the keys the bindings file gives."""
    global DIAGRAM_LEFT, DIAGRAM_RIGHT
    bound = read_bindings(path, scheme)
    for output, name in BUTTON_GLYPHS.items():
        if bound.get(output):
            GLYPHS[name] = short_name(bound[output][0])
    move = [bound.get(output, [''])[0] for output in MOVE_OUTPUTS]
    if all(len(key) == 1 for key in move):
        GLYPHS[STICK_GLYPH] = ''.join(move).upper()
    elif all(move):
        GLYPHS[STICK_GLYPH] = 'Move'

    def row(output, default):
        keys = bound.get(output)
        return '  /  '.join(long_name(key) for key in keys[:2]) if keys else default

    move_row = '  '.join(key.upper() if len(key) == 1 else long_name(key) for key in move) \
        if all(move) else DIAGRAM_LEFT[7]
    DIAGRAM_LEFT = tuple(row(output, default) for output, default in zip(
        ('touchpad_center', 'l2', 'l1', 'pad_up', 'pad_left', 'pad_down', 'pad_right'),
        DIAGRAM_LEFT)) + (move_row,)
    DIAGRAM_RIGHT = tuple(row(output, default) for output, default in zip(
        ('options', 'r2', 'r1', 'triangle', 'circle', 'cross', 'square'),
        DIAGRAM_RIGHT)) + (DIAGRAM_RIGHT[7],)


def write_keyboard(folder):
    folder.mkdir(parents=True, exist_ok=True)
    for name, label in GLYPHS.items():
        write(folder, name, keycap(label))
    for name, image in ((DIAGRAM_HASH, diagram()), (DIAGRAM_LARGE_HASH, diagram(True))):
        (folder / f'{name}.bin').write_bytes(image.tobytes())
        image.save(folder / f'{name}.png')


def main(out_dir, ini=None):
    # <out>/keyboard, one <out>/keyboard_<scheme> for each other scheme and <out>/xbox, the
    # folder names the runtime looks for.
    global DIAGRAM_LEFT, DIAGRAM_RIGHT
    defaults = (dict(GLYPHS), DIAGRAM_LEFT, DIAGRAM_RIGHT)
    for scheme in (None, *SCHEMES):
        GLYPHS.clear()
        GLYPHS.update(defaults[0])
        DIAGRAM_LEFT, DIAGRAM_RIGHT = defaults[1], defaults[2]
        if ini:
            apply_bindings(ini, scheme)
        write_keyboard(Path(out_dir) / ('keyboard_' + scheme.lower() if scheme else 'keyboard'))
    xbox = Path(out_dir) / 'xbox'
    xbox.mkdir(parents=True, exist_ok=True)
    for name, (label, colour) in XBOX.items():
        write(xbox, name, pad_button(label, colour))


if __name__ == '__main__':
    # python glyphs.py <out dir> [--ini <bindings file>]
    main(sys.argv[1], sys.argv[sys.argv.index('--ini') + 1] if '--ini' in sys.argv else None)
