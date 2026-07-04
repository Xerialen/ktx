import os
from pathlib import Path
E6_DIR = Path(os.environ.get("KBOT_E6_DIR", Path(__file__).resolve().parent.parent))
E6_DIR.mkdir(parents=True, exist_ok=True)
lines = ["1 set k_kbot_gj_probe 0", "0 SEAT", "5 set k_kbot_gj_cal 1", "0 set k_kbot_gj_caldir 1"]
pts = []
# +X wall scan from ring side (x=430) across y, at several heights
for y in range(-160, 301, 20):
    for z in (66, 86, 106, 126, 156):
        pts.append((430, y, z))
for (x, y, z) in pts:
    lines.append(f'0.5 set k_kbot_gj_to "{x} {y} {z}"')
with open(E6_DIR / "gj_timeline_wall.txt", "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("points:", len(pts), "est_secs:", 6 + len(pts)*0.5)
