from re_ import *
import re as R
pat=rb'[\x20-\x7e]{4,}\0'
seen=0
for m in R.finditer(pat,img[0xf00000:0x1604000]):
    s=m.group()[:-1]; va=0xf00000+m.start()
    if R.search(rb'(?i)(spawn|summon|debugspawn|createboss|createenemy|createcreature|createai|_ai_|aitarget|\btarget|lockon|aggro|threat|enemy)',s) and R.match(rb'^(shadow_|cp\d+_|r_|ai_|g_|sv_|debug|cl_|game_)',s):
        x=xrefs(va)
        print(hex(va),s.decode(),[hex(a) for a in x][:4])
