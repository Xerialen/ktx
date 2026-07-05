# E6 — gap-crossing strafe-jump play (kbot-0.17.0-gapjump)

One movement technique: a horizontal strafe/speed-jump across dm3's central
hill-pit. Run at the ledge edge with horizontal speed, hop, air-strafe across,
land on the far ledge. **No rocket-jump.** Air-control engine = the E1 c=0
alternating carve (bunnyhop decoupling), used here as a bang-bang steer toward
the landing bearing (kills lateral drift). Wishdir is projected into fmove/smove
through the view yaw at the `bot_movement.c` dir_move_ seam, with the
frame-perfect contact-frame hop latch.

## Cvars (all default OFF / neutral)
- `k_kbot_gapjump` 0/1 — master. **0 = full no-op** (KBot_GapjumpFrame returns
  false on the first check; emitted command is byte-for-byte vanilla).
- `k_kbot_gj_lane` -1 = passive trigger (real feature); 0..3 = trial driver:
  0 ring2quad, 1 quad2ring, 2 ra2ya, 3 ya2ra.
- Tunables: `k_kbot_gj_v0` (launch speed), `k_kbot_gj_steer` (air-steer deadband
  deg), `k_kbot_gj_runup`, `k_kbot_gj_landrad`, `k_kbot_gj_timeout`,
  `k_kbot_gj_cool`, `k_kbot_gj_failz`, `k_kbot_gj_head`, `k_kbot_gj_zone`,
  `k_kbot_gj_to`/`_land` (per-lane geometry override strings).
- Discovery tooling (also gated by k_kbot_gapjump): `k_kbot_gj_probe` (log
  origin+loc of a vanilla-navigating kbot), `k_kbot_gj_cal` + `k_kbot_gj_caldir`
  (0=down / 1=+X / 2=-X / 3=+Y / 4=-Y traceline floor/wall mapper),
  `k_kbot_gj_traj` (per-frame crossing trajectory log).

## Lane geometry (SERVER / setorigin coords; player-rest z, item z = that-24)
Discovered in-lab: dm3.loc/8 = game coords (LocationName divides by 8); the
`.bot` marker frame and the BSP are offset and were NOT trusted. Central pit
floor traced ~-224 (256 deep below the z=32 ledge tops; player rest z=56). The
item line y=248 is BLOCKED by the central pillar; the OPEN corridor is
y=-160..140 (verified wall-free x430->x1024). Both lanes cross this pit +X/-X,
level, ~436u, taking off at the ledge EDGE so the single hop-arc apexes over
the void:
- ring2quad / quad2ring: y=40, ledge edges x=360 <-> x=796.
- ra2ya / ya2ra: y=-130 (southern flank of the same pit; the literal RA-entry
  and YA ledges are separated by a wall, not a clean +X void — see
  e6-evidence/geom_south_map.txt), ledge edges x=360 <-> x=796.

## Tuned parameters (all 4 lanes)
v0=680, steer=1.5, runup=0, landrad=120, timeout=2.5, cool=0.4.

## Results (>=50 trials/lane; e6-evidence/SUMMARY.txt + trial_results.txt)
- ring2quad 51/52 = 98.1%   quad2ring 52/52 = 100%
- ra2ya     51/52 = 98.1%   ya2ra     52/52 = 100%

## Reproduce (WSL2, contained, lab-only)
    GJ_TIMELINE=$PWD/tools/gj_trial_lanes01.txt RUNSECS=130 PORT=28666 bash tools/gj_run.sh
    GJ_TIMELINE=$PWD/tools/gj_trial_lanes23.txt RUNSECS=125 PORT=28669 bash tools/gj_run.sh
Runner clears masters + sv_public 0 before server.cfg advertises; every run
reports `heartbeat/A2A: 0`. Bots seated via qw_min_client.py (`botcmd addkbot`),
editor OFF, dm3's shipped .bot. Geometry timelines: gj_timeline_{probe,trace,
wall,south}.txt; analysis in e6-evidence/scripts/. No-op check: gj_noop_check.txt.

---

# E9 — ACTIVE jump-intent / nav integration (kbot-0.19.0-gapjump-nav)

E8.2 is PASSIVE: the crossing only fires when nav INCIDENTALLY passes the takeoff
lip already aligned + fast (~1.6 attempts/match, ~31% land). E9 makes the launch
conditions TRUE BY CONSTRUCTION: when a kbot's nav GOAL is across a gap-lane and
the bot is on the takeoff side (no enemy near), it DELIBERATELY drives to the lip,
converges south of the central pillar, builds to the launch speed floor, and hands
off to the E8.2 crossing. Never launches a doomed jump (declines instead).

## Gating (k_kbot_gapjump 0 = FULL no-op, unchanged)
- `k_kbot_gj_active` 0/1 — E9 master. 0 = pure E8.2 passive (byte-identical .so
  behaviour to the passive path). Requires k_kbot_gapjump != 0 to do anything.

## Intent detection
`GJ_PickIntentLane`: the GOAL is `self->s.v.goalentity` (the item the bot routes
to; falls back to linked_marker/look_object). For each lane: engage if the bot is
on the takeoff side (along-lane in [-intent_back, 32], within intent_perp of the
lane line, on the ledge z-band) AND the goal projects past the lane midpoint toward
the landing (goal is across the gap). Nearest-to-lip lane wins.

## Approach (`GJ_ApproachFrame`, state GJ_APPROACH)
- Drive ALONG the corridor axis u (not the launch bow -- the bow points partly off
  the ledge into the void and stalls the bot at the edge). Carrot on the corridor
  line, clamped never past the lip, biased `south_bias` units SOUTH (into the open
  corridor, clear of the pillar).
- Circle-accel (`app_build`, reuses the E8 build primitive) toward the carrot when
  under the speed floor with runway behind the lip.
- LAUNCH when grounded at the lip window, fast (vh >= v_req*launch_mul) and aligned,
  with an ASYMMETRIC perp gate: reject NORTH-of-line launches (they clip the pillar
  and fall; every measured LAND had lat<=0). Then hand to GJ_Cross (E8.2 launch).
- DECLINE/ABORT (-> vanilla nav, no pit dive): enemy near, timeout, past the lip
  without launching, or stalled-slow at the lip. A short `cool` re-engage guard
  stops engage/decline flicker so the bot gets a running restart.

## Cvars (E9, all safe at defaults; k_kbot_gj_active 0 = off)
`k_kbot_gj_active` 0, `_intent_back` 384, `_intent_perp` 176, `_intent_zband` 56,
`_apptime` 3.5, `_lookahead` 112, `_launch_win` 48, `_launch_perp` 28,
`_launch_mul` 1.2, `_app_align` 45, `_north_max` 12, `_south_bias` 24,
`_app_build` 1. Reuses `_gate`/`_build_angle`/`_cool`/`_gatelog` from E8.

## Diagnostic (lab-only, contained; --skip-ledger)
    python3 lab/run_bench.py --serverdir .../serverdir --mvdsv .../mvdsv \
      --team-a kbot --team-b frog --candidate-version kbot-0.19.0-gapjump-nav \
      --matches N --jobs 4 --set k_kbot_weak_stack 100 \
      --set k_kbot_gapjump 1 --set k_kbot_gj_active 1 --set k_kbot_gj_gatelog 1 \
      --skip-ledger
Grep server.log for `[gapjump] result=APP_LAUNCH|LAND|FAIL_GAP` counts.
