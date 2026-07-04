import re, collections, os
from pathlib import Path
E6_DIR = Path(os.environ.get("KBOT_E6_DIR", Path(__file__).resolve().parent.parent))
E6_DIR.mkdir(parents=True, exist_ok=True)
LOG=str(E6_DIR / "trace_server.log")
OUT=open(E6_DIR / "floor_maps.txt","w")
def P(*a): __builtins__.print(*a,file=OUT)
rx=re.compile(r"\[gjcal\] to=\((-?\d+) (-?\d+) (-?\d+)\) floorz=(-?\d+) nz=([\d.\-]+) frac=([\d.]+) loc=(.+)$")
floor={}
for line in open(LOG,errors="ignore"):
    m=rx.search(line)
    if not m: continue
    x=int(m.group(1)); y=int(m.group(2)); fz=int(m.group(4)); nz=float(m.group(5)); frac=float(m.group(6))
    floor[(x,y)]=(fz,nz,frac)
def cell(fz,nz,frac):
    # classify: solid ledge (fz in 40..100, nz>0.7) => '#'; deep pit => '.'; wall/no-hit => ' '
    if frac>=0.999: return ':'   # nothing within 1400 down (over deep void or off-map)
    if 40<=fz<=110 and nz>0.7: return '#'   # ledge
    if fz<=-40: return '.'   # pit floor (fell far)
    return 'o'  # intermediate
def render(name, xs, ys):
    P(f"\n=== {name}  ('#'=ledge z40-110, '.'=pit<=-40, 'o'=mid, ':'=void, ' '=no data) ===")
    P("      x:  " + " ".join(f"{x%1000:3d}" for x in xs))
    for y in ys:
        row=[]
        for x in xs:
            if (x,y) in floor:
                row.append(" "+cell(*floor[(x,y)])+" ")
            else:
                row.append("   ")
        P(f"  y={y:5d}: " + " ".join(row))
    # also dump ledge-height detail for '#' cells
def detail(name, xs, ys):
    P(f"\n--- {name} floorz values ---")
    for y in ys:
        vals=[]
        for x in xs:
            if (x,y) in floor:
                fz,nz,frac=floor[(x,y)]
                vals.append(f"{fz:5d}")
            else:
                vals.append("    .")
        P(f"  y={y:5d}: "+" ".join(vals))
rq_x=list(range(250,901,50)); rq_y=list(range(-150,351,50))
render("RING<->QUAD region", rq_x, rq_y)
detail("RING<->QUAD", rq_x, rq_y)
s_x=list(range(380,1141,60)); s_y=list(range(-400,-821,-60))
render("SOUTHERN RA/YA region", s_x, s_y)
detail("SOUTHERN RA/YA", s_x, s_y)
OUT.close();print("done")
