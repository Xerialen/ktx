import re, collections, sys
LOG = sys.argv[1] if len(sys.argv)>1 else "/home/xerial/kbot/gj-lab/server.log"
OUT=open("/mnt/c/Users/benya/kbot-e6/cal_classify.txt","w")
def P(*a): __builtins__.print(*a,file=OUT)
rx=re.compile(r"\[gjcal\] to=\((.*?)\) settle=(-?\d+),(-?\d+),(-?\d+) og=(\d) loc=(.+)$")
byto=collections.OrderedDict()
for line in open(LOG,errors="ignore"):
    m=rx.search(line)
    if not m: continue
    to=m.group(1).strip(); sz=int(m.group(4)); og=int(m.group(5)); loc=m.group(6).strip()
    byto.setdefault(to,[]).append((sz,og,loc))
P(f"{'target':18s} {'minz':>6s} {'solidz':>7s} cls  loc")
for to,rows in byto.items():
    if not to: continue
    minz=min(r[0] for r in rows)
    solid=[r[0] for r in rows if r[1]==1 and r[0]>=30]  # grounded at plausible ledge height
    solidz = max(solid) if solid else None
    # classification
    if minz <= -40:
        cls="GAP "
    elif solidz is not None:
        cls="SOLID"
    else:
        cls="? "
    loc=rows[-1][2]
    P(f"{to:18s} {minz:6d} {str(solidz):>7s} {cls} {loc}")
OUT.close();print("done")
