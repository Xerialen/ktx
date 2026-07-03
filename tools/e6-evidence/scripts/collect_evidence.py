import re, os
logs = ["/mnt/c/Users/benya/kbot-e6/rq_server.log",
        "/mnt/c/Users/benya/kbot-e6/ry2_server.log"]
ev_dir = "/home/xerial/kbot/ktx-gapjump/tools/e6-evidence"
os.makedirs(ev_dir, exist_ok=True)
rx = re.compile(r"(\[gapjump\] lane=\S+ trial=\d+ result=\S+.*)$")
lines = []
for lg in logs:
    for line in open(lg, errors="ignore"):
        m = rx.search(line)
        if m:
            lines.append(m.group(1))
with open(ev_dir + "/trial_results.txt", "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
# summary
summ = []
for L in ["ring2quad", "quad2ring", "ra2ya", "ya2ra"]:
    rows = [x for x in lines if f"lane={L} " in x]
    land = sum(1 for x in rows if "result=LAND" in x)
    fails = {}
    for x in rows:
        if "result=" in x and "LAND" not in x:
            r = x.split("result=")[1].split()[0]
            fails[r] = fails.get(r, 0) + 1
    pct = 100.0*land/len(rows) if rows else 0
    summ.append(f"{L:10s}: {land}/{len(rows)} LAND = {pct:.1f}%  fails={fails}")
with open(ev_dir + "/SUMMARY.txt", "w", newline="\n") as f:
    f.write("E6 gap-jump landing rates (v0=680 steer=1.5 runup=0 landrad=120)\n")
    f.write("\n".join(summ) + "\n")
    f.write("\nlogs: rq_server.log (lanes 0,1), ry2_server.log (lanes 2,3)\n")
print("\n".join(summ))
print("total result lines:", len(lines))
