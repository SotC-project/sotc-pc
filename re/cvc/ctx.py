from re_ import *
import pickle,sys
r,c=pickle.load(open('xr.pkl','rb'))
t=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 4
md.skipdata=False
for a in r.get(t,[])[:n]:
    # disassemble small window via resync
    for s in range(a-0x18,a-0x8):
        ins=list(md.disasm(img[s:a+0x20],s))
        if any(i.address==a for i in ins):
            print('--'); [print(fmt(i)) for i in ins]; break
