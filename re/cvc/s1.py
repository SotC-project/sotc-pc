from re_ import *
for pat in [rb'[A-Za-z_]*_Spawn\0',rb'\+?boss\0',rb'[a-zA-Z_]*[Bb]oss[A-Za-z_]*\0']:
    for va,s in strs(pat)[:60]:
        s=cstr(va)
        print(hex(va),s, [hex(x) for x in xrefs(va)][:8])
