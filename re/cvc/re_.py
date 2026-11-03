import numpy as np, re, sys, capstone
img=open('img.bin','rb').read()
CODE_END=0x1593978
_t=None
def tgt():
    global _t
    if _t is None:
        a=np.frombuffer(img[:CODE_END],dtype=np.uint8)
        n=len(a)-4
        d=(a[:n].astype(np.int64)|(a[1:n+1].astype(np.int64)<<8)|(a[2:n+2].astype(np.int64)<<16)|(a[3:n+3].astype(np.int64)<<24))
        d=np.where(d>=2**31,d-2**32,d)
        _t=np.arange(n,dtype=np.int64)+4+d
    return _t
def strs(pat):
    return [(m.start(),m.group()) for m in re.finditer(pat,img)]
def cstr(va):
    e=img.index(b'\0',va); return img[va:e]
def xrefs(va):
    t=tgt(); idx=np.nonzero(t==va)[0]
    out=[]
    for i in idx:
        i=int(i)
        # lea r, [rip+d]: 48/4c 8d modrm(00 xxx 101)
        if img[i-1]&0xC7==0x05 and img[i-2] in (0x8d,0x8b) : out.append(i-3)
        else: out.append(-i)  # other
    return out
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
def dis(va,n=40,size=None):
    size=size or n*8
    r=[]
    for ins in md.disasm(img[va:va+size],va):
        r.append(ins); 
        if len(r)>=n: break
    return r
def fmt(ins):
    s=f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}"
    m=re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]',ins.op_str)
    if m:
        t=ins.address+ins.size+(int(m.group(2),16)*(1 if m.group(1)=='+' else -1))
        s+=f"   ; ->{t:#x}"
        if t<len(img):
            try:
                c=cstr(t)
                if 2<len(c)<80 and all(32<=b<127 for b in c): s+=f' "{c.decode()}"'
            except: pass
    return s
def funcstart(va):
    # walk back to int3/padding boundary
    p=va
    while p>0:
        if img[p-1]==0xcc or (img[p-1]==0xc3 and img[p-2:p-1]!=b'') and img[p]==0x55: 
            if img[p-1]==0xcc or img[p]==0x55: return p
        p-=1
