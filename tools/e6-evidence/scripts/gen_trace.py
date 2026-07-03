lines = ["1 set k_kbot_gj_probe 0", "0 SEAT", "5 set k_kbot_gj_cal 1"]
pts = []
# Ring<->Quad region: 2D grid to pin ledges + gap extent
for y in range(-150, 351, 50):
    for x in range(250, 901, 50):
        pts.append((x, y, 400))
# Southern RA/YA region
for y in range(-400, -821, -60):
    for x in range(380, 1141, 60):
        pts.append((x, y, 400))
for (x, y, z) in pts:
    lines.append(f'0.55 set k_kbot_gj_to "{x} {y} {z}"')
with open("/mnt/c/Users/benya/kbot-e6/gj_timeline_trace.txt", "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("points:", len(pts), "est_secs:", 6 + len(pts)*0.55)
