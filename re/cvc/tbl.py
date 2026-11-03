from re_ import *
import struct
base=0x16d4010
for k in range(16):
    e=base+k*0x1e8
    out=[]
    for o in range(0,0x1e8,4):
        v=struct.unpack_from('<I',img,e+o)[0]
        q=struct.unpack_from('<Q',img,e+o)[0] if o%8==0 else None
        if q and 0xf00000<q<0x1600000:
            try:
                s=cstr(q)
                if 1<len(s)<64 and all(32<=b<127 for b in s): out.append(f"+{o:#x}:'{s.decode()}'"); continue
            except: pass
        if v and o in (0x0,0x4,0x8,0xc,0x10,0x14,0x28,0x2c,0x30,0x34,0x174,0x178): out.append(f"+{o:#x}={v:#x}")
    print(k,' '.join(out))
