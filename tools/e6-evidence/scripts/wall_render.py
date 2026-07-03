import re
LOG="/mnt/c/Users/benya/kbot-e6/wall_server.log"
OUT=open("/mnt/c/Users/benya/kbot-e6/wall_map.txt","w")
def P(*a): __builtins__.print(*a,file=OUT)
rx=re.compile(r"\[gjcal\] to=\((-?\d+) (-?\d+) (-?\d+)\) dir=1 hit=(-?\d+),(-?\d+),(-?\d+) nz=([\d.\-]+) frac=([\d.]+)")
data={}
for line in open(LOG,errors="ignore"):
    m=rx.search(line)
    if not m: continue
    y=int(m.group(2)); z=int(m.group(3)); hitx=int(m.group(4)); frac=float(m.group(8))
    data[(y,z)]=hitx
zs=[66,86,106,126,156]
P("+X trace from x=430. hit x-position (wall face). >1100 ~ reached far side (open corridor at that height).")
P("  z:     " + " ".join(f"{z:5d}" for z in zs))
for y in range(-160,301,20):
    row=[]
    for z in zs:
        hx=data.get((y,z))
        row.append(f"{hx:5d}" if hx is not None else "    .")
    P(f"  y={y:5d}: " + " ".join(row))
OUT.close();print("done")
