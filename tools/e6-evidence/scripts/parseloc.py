import collections, sys, os
from pathlib import Path
E6_DIR = Path(os.environ.get("KBOT_E6_DIR", Path(__file__).resolve().parent.parent))
E6_DIR.mkdir(parents=True, exist_ok=True)
SERVERDIR = Path(os.environ.get("KBOT_SERVERDIR", Path.home() / "kbot" / "serverdir"))
OUT = open(E6_DIR / "loc_out.txt", "w")
def print(*a, **k):
    __builtins__.print(*a, file=OUT, **k)
TOK = "$loc_name_"
macros = {"ring":"ring","quad":"quad","ra":"ra","ya":"ya","pent":"pent",
          "separator":"/","low":"low","up":"up","high":"high","box":"box",
          "tunnel":"tunnel","lg":"lg","water":"water"}
def expand(s):
    out = s
    while TOK in out:
        i = out.index(TOK); stub = out[i+len(TOK):]
        for k, v in macros.items():
            if stub.startswith(k):
                out = out[:i] + v + out[i+len(TOK)+len(k):]; break
        else:
            out = out[:i] + "?" + out[i+len(TOK):]
    return out
areas = collections.defaultdict(list)
for line in open(SERVERDIR / "ktx" / "locs" / "dm3.loc"):
    p = line.split()
    if len(p) < 4: continue
    try: x, y, z = int(p[0])/8, int(p[1])/8, int(p[2])/8
    except: continue
    name = expand(" ".join(p[3:]))
    key = name.split("/")[0].split()[0] if name else "?"
    areas[key].append((round(x), round(y), round(z), name))
for key in ["ring","quad","ra","ya"]:
    pts = areas.get(key, [])
    if not pts:
        print(f"=== {key}: 0 pts ==="); continue
    xs=[p[0] for p in pts]; ys=[p[1] for p in pts]; zs=[p[2] for p in pts]
    print(f"=== {key}: {len(pts)} pts  x[{min(xs)},{max(xs)}] y[{min(ys)},{max(ys)}] z[{min(zs)},{max(zs)}] ===")
    for p in sorted(pts): print("   ", p)
