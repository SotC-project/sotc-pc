import struct,sys
f=open(r"D:\Games\CUSA08809\eboot.bin","rb").read()
print(f[:8].hex())
if f[:4]==b'\x4f\x15\x3d\x1d':
    hs,ms,fs=struct.unpack_from('<HHQ',f,0xc)
    n,=struct.unpack_from('<H',f,0x18)
    print('self hdrsize',hs,'nseg',n)
    for i in range(n):
        t,o,cs,ds=struct.unpack_from('<QQQQ',f,0x20+i*32)
        print(i,hex(t),hex(o),hex(cs),hex(ds))
    eo=0x20+n*32
else: eo=0
print('elf at',hex(eo),f[eo:eo+4])
phoff,=struct.unpack_from('<Q',f,eo+0x20); phn,=struct.unpack_from('<H',f,eo+0x38)
for i in range(phn):
    p=eo+phoff+i*56
    t,fl,off,va,pa,fsz,msz,al=struct.unpack_from('<IIQQQQQQ',f,p)
    print(i,hex(t),fl,hex(off),hex(va),hex(fsz),hex(msz))
