from re_ import *
import pickle,sys
r,c=pickle.load(open('xr.pkl','rb'))
def fstart(a):
    p=a
    while p>a-0x20000:
        if img[p:p+4]==b'\x55\x48\x89\xe5' and img[p-1] in (0x90,0xcc,0xc3) : return p
        p-=1
for a in sys.argv[1:]:
    a=int(a,16); f=fstart(a)
    refs=c.get(f,[])+r.get(f,[])
    print(hex(a),'func',hex(f),'callers/refs',[hex(x) for x in refs][:10])
