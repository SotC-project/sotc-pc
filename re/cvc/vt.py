from re_ import *
import sys,struct
def q(a): return struct.unpack_from('<Q',img,a)[0]
def rtti_name(ti):
    try:
        n=q(ti+8)
        s=cstr(n)
        if 2<len(s)<200 and all(32<=b<127 for b in s): return s.decode()
    except: pass
for a in sys.argv[1:]:
    a=int(a,16); p=a
    while p>a-0x800:
        v=q(p)
        if 0x1604000<=v<0x3f00000:
            nm=rtti_name(v)
            if nm and q(p-8)<0x100000 or (nm and q(p-8)>=2**63):
                print(hex(a),'vtable@',hex(p+8),'slot',(a-p-8)//8,nm,'off2top',hex(q(p-8))); break
        p-=8
