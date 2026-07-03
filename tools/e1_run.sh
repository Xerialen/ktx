#!/bin/bash
# E1 carve-law verification run (theory doc §7 E1, issue komodobots2#6).
#
# CONTAINMENT (PRD R3, lab-only): the isolated lab serverdir gets a REWRITTEN
# ktx/server.cfg that sets `sv_public 0` and clears masters BEFORE anything
# else, so the stock advertising path (setmaster + heartbeat) never runs. We
# also pass `+set sv_public 0` on the mvdsv command line and re-assert it
# post-boot over the console -- the same belt-and-braces the bench uses
# (lab/run_bench.py build_server_cfg + post_boot_console_cmds). Verify after a
# run: the log must contain NO `Sending heartbeat` and NO `A2A_ACK`.
#
# SEATING: `botcmd` is a CLIENT command, not a server-console command
# (server stdin gives `Unknown command "botcmd"`). We seat the single kbot
# exactly like the bench: the qw_min_client.py shim connects as a spectator
# and sends `botcmd addkbot 20 <team>` as a reliable stringcmd. Arm switching
# (`set k_kbot_e1_mode N`) DOES work over server stdin and stays there.
#
# Runway: ztricks (gen-1 getspeed evidence map). Start/yaw/pass are cvars
# (retune without rebuild).
set -euo pipefail

LAB="${LAB:-$HOME/kbot/e1-lab}"
KTX_SO="${KTX_SO:-$HOME/kbot/ktx-wp22/build/linux-amd64/qwprogs.so}"
MVDSV="${MVDSV:-$HOME/kbot/mvdsv/build/linux-amd64/mvdsv}"
SRC="${SRC:-$HOME/kbot/serverdir}"
MAPSRC="${MAPSRC:-/mnt/c/nQuake/qw/maps/ztricks.bsp}"
SHIM="${SHIM:-$(dirname "$0")/qw_min_client.py}"
PORT="${PORT:-28640}"
E1_START="${E1_START:--1700 1760 -540}"
E1_YAW="${E1_YAW:-0}"
E1_PASS="${E1_PASS:-6}"
E1_TEAM="${E1_TEAM:-red}"
ARM_SECS="${ARM_SECS:-19}"   # ~3 passes of 6 s

[ -x "$MVDSV" ] || { echo "mvdsv not found: $MVDSV"; exit 1; }
[ -f "$KTX_SO" ] || { echo "qwprogs.so not found: $KTX_SO"; exit 1; }
[ -f "$MAPSRC" ] || { echo "ztricks.bsp not found: $MAPSRC"; exit 1; }
[ -f "$SHIM"   ] || { echo "shim not found: $SHIM"; exit 1; }

rm -rf "$LAB"
mkdir -p "$LAB/ktx" "$LAB/id1" "$LAB/qw/maps"

# Read-only reuse of the canonical serverdir via symlinks; our .so on top.
for f in "$SRC"/id1/* ; do ln -s "$f" "$LAB/id1/$(basename "$f")" 2>/dev/null || true; done
for f in "$SRC"/qw/*  ; do
  b=$(basename "$f"); [ "$b" = maps ] && continue   # maps is a real dir (below)
  ln -s "$f" "$LAB/qw/$b" 2>/dev/null || true
done
for f in "$SRC"/ktx/* ; do ln -s "$f" "$LAB/ktx/$(basename "$f")" 2>/dev/null || true; done
# ktx/bots/maps must be a real dir so we can drop a minimal ztricks.bot (below)
# without perturbing the canonical serverdir's read-only bots tree.
rm -f "$LAB/ktx/bots"
mkdir -p "$LAB/ktx/bots/maps"
for f in "$SRC"/ktx/bots/maps/* ; do ln -s "$f" "$LAB/ktx/bots/maps/$(basename "$f")" 2>/dev/null || true; done
# Minimal ztricks.bot: makes LoadBotRoutingFromFile succeed -> map_supported =
# true, so `addkbot` passes the map-support gate with EDITOR MODE OFF (editor
# mode swaps FrogbotsCommand to a marker-only command table that has NO
# addkbot -- the reason bot seating silently failed). E1 drives the bot
# directly (teleport + per-frame carve), so marker quality is irrelevant; a
# short walkable chain down the runway is enough. The cherry-picked route/goal
# NULL guards keep the frogbot think path safe on this sparse graph.
cat > "$LAB/ktx/bots/maps/ztricks.bot" <<'BOT'
CreateMarker -1700 1760 -540
CreateMarker -1400 1700 -540
CreateMarker -1100 1620 -540
CreateMarker -700 1560 -540
CreateMarker -300 1500 -540
SetMarkerPath 1 0 2
SetMarkerPath 2 0 3
SetMarkerPath 2 1 1
SetMarkerPath 3 0 4
SetMarkerPath 3 1 2
SetMarkerPath 4 0 5
SetMarkerPath 4 1 3
SetMarkerPath 5 0 4
BOT
# qw/maps is already a real dir (mkdir -p above); fill with symlinked canonical maps.
for f in "$SRC"/qw/maps/* ; do ln -s "$f" "$LAB/qw/maps/$(basename "$f")" 2>/dev/null || true; done
rm -f "$LAB/ktx/qwprogs.so"; cp "$KTX_SO" "$LAB/ktx/qwprogs.so"
cp "$MAPSRC" "$LAB/qw/maps/ztricks.bsp"

# CONTAINED server.cfg: replace the stock one (which does `setmaster ...` and
# would advertise). Clear masters + sv_public 0 FIRST (containment), then the
# stock mod sub-cfgs (mvdsv.cfg/ktx.cfg are needed for KTX), then re-assert
# containment AND the client-accept invariants that must WIN over the
# sub-cfgs: sv_getrealip 0 + sv_login 0 -- without them the shim's spectator
# connect stalls in the getrealip `ip 0 ...` handshake and never signs on
# (so the botcmd addkbot is never sent). Same set the bench uses.
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
set k_defmap ztricks
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
set k_kbot_e1_mode 0
set k_kbot_e1_start "$E1_START"
set k_kbot_e1_yaw $E1_YAW
set k_kbot_e1_pass $E1_PASS
CFG

echo "[e1_run] lab=$LAB port=$PORT so=$(md5sum "$LAB/ktx/qwprogs.so" | cut -c1-8)"

# mvdsv stdin over a FIFO so we can time console commands across the run.
FIFO="$LAB/console.fifo"
rm -f "$FIFO"; mkfifo "$FIFO"

( cd "$LAB" && "$MVDSV" -port "$PORT" -game ktx -mem 64 +set sv_public 0 \
    +exec server.cfg < "$FIFO" ) > "$LAB/server.log" 2>&1 &
MVDSV_PID=$!
# Hold the FIFO open (fd 9) so the reader never sees EOF between commands.
exec 9>"$FIFO"

cleanup() { exec 9>&- 2>/dev/null || true; kill "$MVDSV_PID" 2>/dev/null || true; rm -f "$FIFO"; }
trap cleanup EXIT

# Post-boot re-assert (belt-and-braces containment + client-accept), then map.
sleep 8
echo "sv_public 0" >&9
echo "sv_getrealip 0" >&9
echo "sv_login 0" >&9
echo "map ztricks" >&9        # load the runway
sleep 6

# Seat ONE kbot via the client shim (botcmd is a client command). Keep it
# connected for the whole run so the bot stays seated (bench pattern).
TOTAL=$(( 3 + ARM_SECS + 2 + ARM_SECS + 3 ))
python3 "$SHIM" "$PORT" --host 127.0.0.1 --name Komodo-e1 --spectator \
    --bot-count 0 --botcmd "addkbot 20 $E1_TEAM" --botcmd-delay 2 \
    --run-for "$TOTAL" > "$LAB/shim.log" 2>&1 &
SHIM_PID=$!

sleep 3;  echo "k_kbot_e1_mode 1" >&9      # ARM A: c=0 alternating carve
sleep "$ARM_SECS"; echo "k_kbot_e1_mode 0" >&9
sleep 2;  echo "k_kbot_e1_mode 2" >&9      # ARM B: gen-1 mode-13 K=26
sleep "$ARM_SECS"; echo "k_kbot_e1_mode 0" >&9
sleep 3;  echo "quit" >&9

wait "$SHIM_PID" 2>/dev/null || true
sleep 1
cleanup
trap - EXIT

HB=$(grep -cE "Sending heartbeat|A2A_ACK" "$LAB/server.log" 2>/dev/null || echo 0)
# [e1] lines are prefixed by the mvdsv timestamp, so match the tag anywhere.
E1=$(grep -cF "[e1] t=" "$LAB/server.log" 2>/dev/null || echo 0)
echo "[e1_run] done. heartbeat/A2A lines: $HB (must be 0); [e1] telemetry lines: $E1"
echo "[e1_run] parse: python3 tools/e1_parse.py $LAB/server.log"
