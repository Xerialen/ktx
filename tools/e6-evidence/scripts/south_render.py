import re, os
from pathlib import Path
E6_DIR = Path(os.environ.get("KBOT_E6_DIR", Path(__file__).resolve().parent.parent))
E6_DIR.mkdir(parents=True, exist_ok=True)
LOG=str(E6_DIR / "south_server.log")
OUT=open(E6_DIR / "south_map.txt","w")
def P(*a): __builtins__.print(*a,file=OUT)
rx=re.compile(r"\[gjcal\] to=\((-?\d+) (-?\d+) (-?\d+)\) dir=0 hit=(-?\d+),(-?\d+),(-?\d+) nz=([\d.\-]+) frac=([\d.]+) loc=(.+)$")
floor={}; locs={}
for line in open(LOG,errors="ignore"):
    m=rx.search(line)
    if not m: continue
    x=int(m.group(1)); y=int(m.group(2)); fz=int(m.group(6)); frac=float(m.group(8)); loc=m.group(9).strip()
    floor[(x,y)]=(fz,frac); locs[(x,y)]=loc
xs=list(range(440,1101,40)); ys=list(range(-440,-821,-40))
def cell(fz,frac):
    if frac>=0.999: return ' : '   # deep void (nothing within 1400)
    if 40<=fz<=110: return ' # '    # ledge z40-110
    if fz<=-40: return ' . '        # pit
    if fz>=130: return ' ^ '        # start inside solid (overhead) -> ledge above
    return ' o '
P("SOUTHERN floor map ('#'=ledge40-110 '.'=pit<=-40 '^'=solid-at/above-140 ':'=deep-void 'o'=mid)")
P("     x: " + "".join(f"{x%1000:3d}" for x in xs))
for y in ys:
    row=""
    for x in xs:
        row += cell(*floor[(x,y)]) if (x,y) in floor else "   "
    P(f" y={y:5d}:"+row)
P("\n--- floorz values ---")
P("     x: " + "".join(f"{x%1000:5d}" for x in xs))
for y in ys:
    row=""
    for x in xs:
        if (x,y) in floor: row+=f"{floor[(x,y)][0]:5d}"
        else: row+="    ."
    P(f" y={y:5d}:"+row)
OUT.close();print("done")
