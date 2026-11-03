from re_ import *
import sys,struct
d=img[0x1604000:]
for a in sys.argv[1:]:
    v=struct.pack('<Q',int(a,16)); hits=[]; i=d.find(v)
    while i>=0 and len(hits)<6: hits.append(hex(0x1604000+i)); i=d.find(v,i+1)
    print(a,hits)
