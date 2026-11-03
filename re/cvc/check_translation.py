"""Check the decoded type-0 translation layout used by teleport/floor probes."""
from pathlib import Path
import struct

image = (Path(__file__).parent / 'img.bin').read_bytes()
# GetCharacter returns object+0x120; rebuild passes character+0xc0 to GetTransform.
assert image[0x61151f:0x611526] == bytes.fromhex('4881c720010000')
assert image[0x66120c:0x661214] == bytes.fromhex('4d8db424c0000000')
# Type zero copies translation from transform+0x10/+0x14/+0x18.
assert image[0x5f6360:0x5f6363] == bytes.fromhex('8b4310')
assert image[0x5f6367:0x5f636a] == bytes.fromhex('8b4314')
assert image[0x5f636e:0x5f6371] == bytes.fromhex('8b4318')
# The native consumer tests character+0x140 and clears it after applying the delta.
assert image[0x661b2e:0x661b35] == bytes.fromhex('80bb4001000000')
assert image[0x661b5f:0x661b66] == bytes.fromhex('c6834001000000')
translation = 0x120 + 0xc0 + 0x10
assert translation == 0x1f0
actor = bytearray(0x300)
struct.pack_into('<4f', actor, translation, 10, 20, 30, 1)
struct.pack_into('<I', actor, 0x20c, 0)
for axis, delta in enumerate((500, 0, 300)):
    offset = translation + axis * 4
    struct.pack_into('<f', actor, offset, struct.unpack_from('<f', actor, offset)[0] + delta)
assert struct.unpack_from('<4f', actor, translation) == (510, 20, 330, 1)
assert struct.unpack_from('<I', actor, 0x20c)[0] == 0
source = (Path(__file__).parents[2] / 'runtime/src/sdl_window.cpp').read_text()
assert '0x1f8, 0x1410, 0x14d0' not in source
assert '{0x1f4}' in source
assert '*reinterpret_cast<const float*>(object + 0x1f4)' in source
print('Translation +0x1f0; Y +0x1f4; homogeneous W and transform type preserved')
