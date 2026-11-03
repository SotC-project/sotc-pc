from re_ import *
import collections, pickle
# linear sweep of whole code, record rip-relative targets and call targets
md.skipdata=True
refs=collections.defaultdict(list); calls=collections.defaultdict(list)
for ins in md.disasm(img[0x4000:0xf00000],0x4000):
    if ins.mnemonic=='call' and ins.op_str.startswith('0x'):
        calls[int(ins.op_str,16)].append(ins.address)
    elif 'rip' in ins.op_str:
        m=re.search(r'\[rip ([+-]) (0x[0-9a-f]+)\]',ins.op_str)
        if m:
            t=ins.address+ins.size+(int(m.group(2),16)*(1 if m.group(1)=='+' else -1))
            refs[t].append(ins.address)
pickle.dump((dict(refs),dict(calls)),open('xr.pkl','wb'))
print(len(refs),len(calls))
