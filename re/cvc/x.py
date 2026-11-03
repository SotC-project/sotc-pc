from re_ import *
import sys
t=tgt()
for a in sys.argv[1:]:
    va=int(a,16)
    # any instr with rip-rel disp; disp field located at i, instr end = i+4 (+imm possible). match t==va or t==va-imm(1/4)
    hits=set()
    for adj in (0,1,4,2):
        for i in np.nonzero(t==va-adj)[0]: hits.add((int(i),adj))
    print(a,[ (hex(i),adj) for i,adj in sorted(hits)][:40])
