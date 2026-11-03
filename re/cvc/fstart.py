from re_ import *
import sys
def start(a):
    p=a
    while p>a-0x3000:
        if img[p:p+4]==bytes.fromhex('554889e5') and img[p-1] in (0x90,0xcc,0xc3): return p
        p-=1
def show(a, before=30, after=8):
    s=start(a); ins=[]
    for i in md.disasm(img[s:a+64],s):
        ins.append(i)
        if i.address>=a: break
    idx=len(ins)-1
    print(f'func {s:#x}')
    for i in ins[max(0,idx-before):]: print(fmt(i))
    for i in dis(ins[-1].address+ins[-1].size, after): print(fmt(i))
if __name__=='__main__':
    for x in sys.argv[1:]: show(int(x,16)); print('====')
