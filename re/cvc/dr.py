# disassemble window ending at target, trying alignment by resync
from re_ import *
import sys
t=int(sys.argv[1],16); back=int(sys.argv[2],0) if len(sys.argv)>2 else 0x80; fwd=int(sys.argv[3],0) if len(sys.argv)>3 else 0x80
for s in range(t-back,t-back+16):
    ins=list(md.disasm(img[s:t+fwd],s))
    if any(i.address==t for i in ins):
        for i in ins: print(fmt(i))
        break
