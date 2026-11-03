from re_ import *
import sys
a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
for i in dis(a,n,n*10): print(fmt(i))
