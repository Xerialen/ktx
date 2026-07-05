#!/bin/bash
# E6 gap-jump lab runner (dm3). Contained, lab-only (PRD R3): clears masters +
# sv_public 0 before the stock server.cfg can advertise, re-asserts post-boot,
# and the run reports heartbeat/A2A counts (must be 0). Seats ONE kbot via the
# qw_min_client.py shim (botcmd addkbot). Uses dm3's REAL shipped .bot (editor
# OFF). Console command timeline read from $GJ_TIMELINE (lines: "<delay_s> <cmd>";
# cmd == SEAT seats the kbot at that point). Server log -> $LAB/server.log.
set -euo pipefail

LAB="${LAB:-$HOME/kbot/gj-lab}"
KTX_SO="${KTX_SO:-$HOME/kbot/ktx-gapjump/build/linux-amd64/qwprogs.so}"
MVDSV="${MVDSV:-$HOME/kbot/mvdsv/build/linux-amd64/mvdsv}"
SRC="${SRC:-$HOME/kbot/serverdir}"
MAP="${MAP:-dm3}"
SHIM="${SHIM:-$(dirname "$0")/qw_min_client.py}"
PORT="${PORT:-28650}"
TEAM="${TEAM:-red}"
TIMELINE="${GJ_TIMELINE:?set GJ_TIMELINE to a timeline file}"
RUNSECS="${RUNSECS:-60}"

[ -x "$MVDSV" ] || { echo "mvdsv not found: $MVDSV"; exit 1; }
[ -f "$KTX_SO" ] || { echo "qwprogs.so not found: $KTX_SO"; exit 1; }
[ -f "$SHIM"   ] || { echo "shim not found: $SHIM"; exit 1; }
[ -f "$TIMELINE" ] || { echo "timeline not found: $TIMELINE"; exit 1; }
[ -f "$SRC/qw/maps/$MAP.bsp" ] || { echo "map missing: $SRC/qw/maps/$MAP.bsp"; exit 1; }

rm -rf "$LAB"
mkdir -p "$LAB/ktx" "$LAB/id1" "$LAB/qw/maps"
for f in "$SRC"/id1/* ; do ln -s "$f" "$LAB/id1/$(basename "$f")" 2>/dev/null || true; done
for f in "$SRC"/qw/* ; do
  b=$(basename "$f"); [ "$b" = maps ] && continue
  ln -s "$f" "$LAB/qw/$b" 2>/dev/null || true
done
for f in "$SRC"/qw/maps/* ; do ln -s "$f" "$LAB/qw/maps/$(basename "$f")" 2>/dev/null || true; done
for f in "$SRC"/ktx/* ; do ln -s "$f" "$LAB/ktx/$(basename "$f")" 2>/dev/null || true; done
rm -f "$LAB/ktx/qwprogs.so"; cp "$KTX_SO" "$LAB/ktx/qwprogs.so"

# CONTAINED server.cfg (same belt-and-braces as e1_run.sh).
rm -f "$LAB/ktx/server.cfg"
cat > "$LAB/ktx/server.cfg" <<CFG
sv_public 0
setmaster
exec mvdsv.cfg
exec ktx.cfg
sv_public 0
sv_getrealip 0
sv_login 0
set k_defmode 4on4
set k_mode 2
set k_defmap $MAP
set k_count 0
set k_auto_xonx 0
set k_lockmap 1
set k_membercount 3
set k_lockmin 1
set k_lockmax 2
coop 0
maxclients 9
set k_maxclients 8
deathmatch 1
teamplay 2
fraglimit 0
samelevel 1
set k_fb_enabled 1
set k_fb_options 0
set k_fb_skill 20
set k_fb_autoadd_limit 0
set k_fb_autoremove_at 0
set k_fb_auto_delay 1
set k_matchless 1
set k_idletime 0
set k_kbot_gapjump 1
set k_kbot_gj_lane -1
set k_kbot_gj_probe 0
CFG

echo "[gj_run] lab=$LAB port=$PORT map=$MAP so=$(md5sum "$LAB/ktx/qwprogs.so" | cut -c1-12)"

FIFO="$LAB/console.fifo"; rm -f "$FIFO"; mkfifo "$FIFO"
( cd "$LAB" && "$MVDSV" -port "$PORT" -game ktx -mem 64 +set sv_public 0 \
    +exec server.cfg < "$FIFO" ) > "$LAB/server.log" 2>&1 &
MVDSV_PID=$!
exec 9>"$FIFO"
cleanup() { exec 9>&- 2>/dev/null || true; kill "$MVDSV_PID" 2>/dev/null || true; rm -f "$FIFO"; }
trap cleanup EXIT

sleep 8
echo "sv_public 0" >&9
echo "sv_getrealip 0" >&9
echo "sv_login 0" >&9
echo "map $MAP" >&9
sleep 6

SHIM_PID=""
# Walk the timeline.
while IFS= read -r line; do
  [ -z "$line" ] && continue
  case "$line" in \#*) continue;; esac
  delay="${line%% *}"
  cmd="${line#* }"
  sleep "$delay"
  if [ "$cmd" = "SEAT" ]; then
    python3 "$SHIM" "$PORT" --host 127.0.0.1 --name Komodo-gj --spectator \
        --bot-count 0 --botcmd "addkbot 20 $TEAM" --botcmd-delay 2 \
        --run-for "$RUNSECS" > "$LAB/shim.log" 2>&1 &
    SHIM_PID=$!
  else
    echo "$cmd" >&9
  fi
done < "$TIMELINE"

[ -n "$SHIM_PID" ] && { wait "$SHIM_PID" 2>/dev/null || true; }
sleep 1
echo "quit" >&9
sleep 1
cleanup
trap - EXIT

HB=$(grep -cE "Sending heartbeat|A2A_ACK" "$LAB/server.log" 2>/dev/null || echo 0)
GP=$(grep -cF "[gjprobe]" "$LAB/server.log" 2>/dev/null || echo 0)
GJ=$(grep -cF "[gapjump] lane=" "$LAB/server.log" 2>/dev/null || echo 0)
echo "[gj_run] done. heartbeat/A2A: $HB (must be 0)  gjprobe: $GP  gapjump: $GJ"
