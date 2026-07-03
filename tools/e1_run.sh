#!/bin/bash
# E1 carve-law verification run (theory doc §7 E1, issue komodobots2#6).
#
# Creates an ISOLATED lab serverdir (never touches ~/kbot/serverdir contents;
# read-only symlinks only), copies the ztricks.bsp runway map from the
# Windows nQuake install, launches mvdsv on :28640, spawns ONE kbot, runs
# ARM A (c=0 alternating carve) and ARM B (gen-1 mode-13 K=26) for ~3x6 s
# passes each via the E1 cvars, then kills the server. Console output
# (incl. the [e1] telemetry) lands in $LAB/server.log; parse with
# tools/e1_parse.py.
#
# Runway: ztricks -- gen-1's getspeed evidence map (komodobots
# experiments/ktx_moveprobe/evidence/getandmaintainspeed-reference: human
# start (-1117,1536,-552) yaw 315; the flat area spans x -1750..-67 at
# y 1500..2100, z floor ~ -576). Default start (-1700 1760 -540) heading
# yaw 0 (+x) gives ~1650 qu of straight free line (~3.2 s at target curve);
# the 2 s / ~490 ups acceptance point fits in one pass, the 4 s point is
# lane-capped -- the slope (~86 qu/s^2 at 400) is the load-bearing check.
# Retune with E1_START/E1_YAW env overrides, no rebuild needed.
set -euo pipefail

LAB="${LAB:-$HOME/kbot/e1-lab}"
KTX_SO="${KTX_SO:-$HOME/kbot/ktx-wp22/build/linux-amd64/qwprogs.so}"
MVDSV="${MVDSV:-$HOME/kbot/mvdsv/build/linux-amd64/mvdsv}"
SRC="${SRC:-$HOME/kbot/serverdir}"
MAPSRC="${MAPSRC:-/mnt/c/nQuake/qw/maps/ztricks.bsp}"
PORT="${PORT:-28640}"
E1_START="${E1_START:--1700 1760 -540}"
E1_YAW="${E1_YAW:-0}"
E1_PASS="${E1_PASS:-6}"
ARM_SECS="${ARM_SECS:-19}"   # ~3 passes of 6 s

[ -x "$MVDSV" ] || { echo "mvdsv not found: $MVDSV"; exit 1; }
[ -f "$KTX_SO" ] || { echo "qwprogs.so not found: $KTX_SO"; exit 1; }
[ -f "$MAPSRC" ] || { echo "ztricks.bsp not found: $MAPSRC"; exit 1; }

rm -rf "$LAB"
mkdir -p "$LAB/ktx" "$LAB/id1" "$LAB/qw/maps"

# Read-only reuse of the canonical serverdir via symlinks; our .so on top.
for f in "$SRC"/id1/* ; do ln -s "$f" "$LAB/id1/$(basename "$f")" 2>/dev/null || true; done
for f in "$SRC"/qw/*  ; do ln -s "$f" "$LAB/qw/$(basename "$f")"  2>/dev/null || true; done
for f in "$SRC"/ktx/* ; do ln -s "$f" "$LAB/ktx/$(basename "$f")" 2>/dev/null || true; done
# qw/maps must be a real dir with the runway map; keep canonical maps too.
rm -f "$LAB/qw/maps"; mkdir -p "$LAB/qw/maps"
for f in "$SRC"/qw/maps/* ; do ln -s "$f" "$LAB/qw/maps/$(basename "$f")" 2>/dev/null || true; done
rm -f "$LAB/ktx/qwprogs.so"; cp "$KTX_SO" "$LAB/ktx/qwprogs.so"
cp "$MAPSRC" "$LAB/qw/maps/ztricks.bsp"

echo "[e1_run] lab=$LAB port=$PORT so=$(md5sum "$LAB/ktx/qwprogs.so" | cut -c1-8)"

# Drive the server through stdin (mvdsv console). Timings are generous:
# world spawn, map change, bot join, then the two arms.
{
  sleep 8;  echo "map ztricks"
  sleep 6;  echo "botcmd addkbot"
  sleep 3;  echo "k_kbot_e1_mode 1"      # ARM A: c=0 alternating carve
  sleep "$ARM_SECS"; echo "k_kbot_e1_mode 0"
  sleep 2;  echo "k_kbot_e1_mode 2"      # ARM B: gen-1 mode-13 K=26
  sleep "$ARM_SECS"; echo "k_kbot_e1_mode 0"
  sleep 1;  echo "quit"
} | (cd "$LAB" && "$MVDSV" -port "$PORT" -game ktx -mem 64 \
      +set k_fb_enabled 1 \
      +set k_fb_options 2 \
      +set k_fb_autoadd_limit 0 \
      +set k_matchless 1 \
      +set k_kbot_e1_mode 0 \
      +set k_kbot_e1_start "$E1_START" \
      +set k_kbot_e1_yaw "$E1_YAW" \
      +set k_kbot_e1_pass "$E1_PASS" \
      ) > "$LAB/server.log" 2>&1 || true

echo "[e1_run] done. log: $LAB/server.log ($(grep -c '^\[e1\]' "$LAB/server.log" 2>/dev/null || echo 0) telemetry lines)"
echo "[e1_run] parse: python3 tools/e1_parse.py $LAB/server.log"
