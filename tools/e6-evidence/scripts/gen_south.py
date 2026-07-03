lines = ["1 set k_kbot_gj_probe 0", "0 SEAT", "5 set k_kbot_gj_cal 1", "0 set k_kbot_gj_caldir 0"]
pts = []
# Southern floor map, start z=140 (below the RA/YA overhead structures)
for y in range(-440, -821, -40):
    for x in range(440, 1101, 40):
        pts.append((x, y, 140))
for (x, y, z) in pts:
    lines.append(f'0.45 set k_kbot_gj_to "{x} {y} {z}"')
with open("/mnt/c/Users/benya/kbot-e6/gj_timeline_south.txt", "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print("points:", len(pts), "est_secs:", 6 + len(pts)*0.45)
