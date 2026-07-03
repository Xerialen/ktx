#!/usr/bin/env python3
"""E1 telemetry parser (theory doc §7 E1, issue komodobots2#6).

Reads a server log containing [e1] lines, writes one speed-time-series CSV
per arm (pass, t_rel, speed, yaw_pin, view_yaw, onground, x, y, z) and
prints the acceptance summary:

  ARM A (mode 1, c=0 alternating carve):
    ~320 -> ~490 ups at ~2 s and ~610 at 4 s (both +/-10%), yaw variance ~0.
  ARM B (mode 2, K=26 single-sided) must not beat ARM A.

Usage: python3 tools/e1_parse.py <server.log> [outdir]
"""
import csv
import re
import statistics
import sys

LINE = re.compile(
    r"\[e1\] t=(?P<t>[\d.]+) mode=(?P<mode>\d) pass=(?P<p>\d+) speed=(?P<s>[\d.]+) "
    r"yaw=(?P<yaw>-?[\d.]+) vyaw=(?P<vyaw>-?[\d.]+) og=(?P<og>\d) "
    r"pos=(?P<x>-?\d+),(?P<y>-?\d+),(?P<z>-?\d+)")

ARM_NAMES = {1: "arm_a_carve", 2: "arm_b_k26"}


def speed_at(series, t_rel, tol=0.15):
    """Speed sample closest to t_rel within tol, else None."""
    best = None
    for row in series:
        d = abs(row["t_rel"] - t_rel)
        if d <= tol and (best is None or d < abs(best["t_rel"] - t_rel)):
            best = row
    return best["speed"] if best else None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    log = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."

    arms = {1: [], 2: []}   # mode -> list of rows
    pass_start = {}         # (mode, pass) -> first t
    for line in open(log, errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        mode = int(m.group("mode"))
        pnum = int(m.group("p"))
        t = float(m.group("t"))
        key = (mode, pnum)
        if key not in pass_start:
            pass_start[key] = t
        arms[mode].append({
            "pass": pnum,
            "t_rel": round(t - pass_start[key], 3),
            "speed": float(m.group("s")),
            "yaw_pin": float(m.group("yaw")),
            "view_yaw": float(m.group("vyaw")),
            "onground": int(m.group("og")),
            "x": int(m.group("x")), "y": int(m.group("y")), "z": int(m.group("z")),
        })

    results = {}
    for mode, rows in arms.items():
        if not rows:
            continue
        name = ARM_NAMES[mode]
        path = f"{outdir}/e1_{name}.csv"
        with open(path, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        # per-pass curve points
        passes = sorted(set(r["pass"] for r in rows))
        at2, at4, peak = [], [], []
        vyaws = [r["view_yaw"] for r in rows]
        for p in passes:
            series = [r for r in rows if r["pass"] == p]
            s2 = speed_at(series, 2.0)
            s4 = speed_at(series, 4.0)
            if s2 is not None:
                at2.append(s2)
            if s4 is not None:
                at4.append(s4)
            peak.append(max(r["speed"] for r in series))
        results[mode] = {
            "name": name, "csv": path, "passes": len(passes),
            "at2": at2, "at4": at4, "peak": peak,
            "yaw_var": statistics.pvariance(vyaws) if len(vyaws) > 1 else 0.0,
        }
        print(f"{name}: {len(rows)} samples, {len(passes)} passes -> {path}")
        print(f"  speed@2s per pass: {[round(v,1) for v in at2]}")
        print(f"  speed@4s per pass: {[round(v,1) for v in at4]}")
        print(f"  peak per pass:     {[round(v,1) for v in peak]}")
        print(f"  view-yaw variance: {results[mode]['yaw_var']:.6f}")

    # Acceptance (issue #6)
    print("\n=== E1 acceptance ===")
    ok = True
    a = results.get(1)
    if not a or not a["at2"]:
        print("FAIL: no ARM A data")
        sys.exit(2)
    m2 = statistics.mean(a["at2"])
    in2 = 441.0 <= m2 <= 539.0
    print(f"ARM A speed@2s mean {m2:.1f} (target 490 +/-10%): {'PASS' if in2 else 'FAIL'}")
    ok &= in2
    if a["at4"]:
        m4 = statistics.mean(a["at4"])
        in4 = 549.0 <= m4 <= 671.0
        print(f"ARM A speed@4s mean {m4:.1f} (target 610 +/-10%): {'PASS' if in4 else 'FAIL'}")
        ok &= in4
    else:
        print("ARM A speed@4s: NO DATA (lane-capped?) -- judge the slope from the CSV")
    yv_ok = a["yaw_var"] < 0.01
    print(f"ARM A view-yaw variance {a['yaw_var']:.6f} (< 0.01): {'PASS' if yv_ok else 'FAIL'}")
    ok &= yv_ok
    b = results.get(2)
    if b and b["at2"] and a["at2"]:
        b_beats = statistics.mean(b["at2"]) > m2
        print(f"ARM B speed@2s mean {statistics.mean(b['at2']):.1f} "
              f"{'BEATS ARM A: FAIL' if b_beats else '<= ARM A: PASS'}")
        ok &= not b_beats
    print(f"\nE1 verdict: {'PASS -- carve law confirmed live' if ok else 'FAIL -- see curves (falsifies E3 premise if ARM A underdelivers)'}")


if __name__ == "__main__":
    main()
