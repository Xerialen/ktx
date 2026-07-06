# Komodobot (kbot) server cvars

Reference for every `k_kbot_*` cvar registered in `src/world.c` (126 total:
101 literal registrations + the 25-cvar tactical-dial family generated in a
loop). All komodobot mechanisms are **byte-neutral by default**: with every
cvar at its default, a komodobot behaves like the module's baseline and stock
frogbots are never affected (every seam gates on `fb.kbot`). Cvars are read
live (most through 1 s caches) — set them from the server cfg or console
without a rebuild.

Conventions used below:

- **0/1** = off/on toggle. **0 = off** means the whole mechanism short-circuits.
- *(units)* refer to Quake units (qu), seconds (s) or degrees (deg).
- "kbot-only" is implied everywhere: none of these change stock frogbot behavior.

---

## Identity & roster (`kbot_main.c`)

Owner roster rule 2026-07-06: team **komo**, color **3**, names
**hib/dag/Angua/Rock**. Applied in `KBot_MarkBot` by kbot join order (slot 1–4);
the same slot index feeds the per-bot dial overrides below.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_name1`..`4` | `hib` / `dag` / `Angua` / `Rock` | Player name for kbot join-order slot 1–4. Empty = keep the frogbot name (a `kb:` prefix is stamped as fallback identity). |
| `k_kbot_color` | `3` | Top+bottom color applied to every kbot. |
| `k_kbot_team` | `komo` | Team name (registered for interface parity; team seating is normally done by the bench/launcher). |
| `k_kbot_version_suffix` | *(empty)* | Free-text suffix appended to the `[kbot] ... version=` identity stamp in the server log — used by the lab to tag builds/arms. |

## Discipline tunables (`kbot_main.c`, WP3.5)

`KBot_AvoidFights` weak-stack retreat gate. Defaults reproduce
`kbot-0.5.0-discipline` exactly.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_weak_stack` | `70` | Health+armor sum under which a kbot counts as weak and avoids engagements. Scaled at runtime by the engage dial's weak-scale (see dials). |
| `k_kbot_weak_rockets` | `3` | Owning at least this many rockets (with RL) exempts the bot from weak-avoidance. |
| `k_kbot_weak_cells` | `15` | Owning at least this many cells (with LG) exempts the bot from weak-avoidance. |

## Decision log (`kbot_dlog.c`, KDLOG)

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_dlog` | `0` | Structured decision log to the server log (`KDLOG` lines: goal candidates/chosen, enemy-target changes, evade flips, play/lane events). 0 off, 1 komodobots only, 2 all bots. High volume — lab use. |

## Legacy TDM v1 levers (`kbot_main.c` / `bot_botenemy.c`)

From the 2026-07-05 tactical-decision-model v1. Superseded by the UTBYTE model
but kept as independent levers.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_commit` | `0` | Engagement commit window (s): after choosing to fight, stick with the target for this long instead of re-evaluating every think. 0 off. Tournament used `1.5`. |
| `k_kbot_route_focus` | `0` | REFUTED lever (kept for reference): suppress enemy hunting while on an item route by clearing the enemy. Clearing `s.v.enemy` also killed dodge/evade → −47..−55 margins. Leave 0. |
| `k_kbot_finish_hp` | `40` | Estimated enemy health under which a finish-off engagement is always allowed (overrides route focus / weak gates). |
| `k_kbot_dive_gate` | `0` | 0/1: decline water dives while carrying a valuable stack (pre-HARVEST water lever; B1 below is the tuned version). |

## Tactical decision models (`kbot_models.c`)

Data-filled from the Book/]sr[ dm3 win corpus. The models scale frogbot goal
and hunt **desirability costs only** — they never touch item desire values (the
project's hard rule R2).

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_model` | `0` | 0 off (byte-neutral), 1 TDM, 2 KAPTEN (role gradient ANKARE/JAGARE via 2 s reallocation tick), 3 **UTBYTE** (tournament winner: exchange decisions TA/POKA/VÄGRA/FINISH from stack-class matchup tables; also the base the engage dial steps). |
| `k_kbot_model_red` | `0` | Model override for the red team (model face-offs). 0 = use `k_kbot_model`. |
| `k_kbot_model_blue` | `0` | Model override for the blue team. |

## HARVEST possession layer (`kbot_harvest.c`)

Retention on top of UTBYTE — cost shaping only, one cvar per mechanism, all
default off. Spec: komodobots2 `docs/specs/2026-07-06-harvest-model-design.md`.
Carried value **V** = stack/250 × firepower weight (RL/LG 1.0, GL/SNG 0.5,
low 0.15; powerup runner ×0.3).

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_harvest_route` | `0` | **B1** water-route penalty: base path-score penalty (spec start `2.5`) on `T_WATER` markers in `EvalPath`, scaled by V — stacked carriers route around water. 0 off. |
| `k_kbot_harvest_threat` | `0` | **B2** place-threat penalty: base weight (spec start `2.0`) × per-marker death memory (+1.0 per kbot death, ~1/120 s decay) × known-enemy class weight (700 qu falloff; weak enemies weigh 0 = press on) × V. Enemy quad carrier ⇒ max penalty. 0 off. |
| `k_kbot_harvest_anchor` | `0` | **B3** zone anchoring: `goal_time` inflation factor for out-of-zone goals for the blackboard-elected ANKARE (spec start `1.4`; other armed+ bots get half the inflation; powerups/big weapons exempt). ≤1 off. |
| `k_kbot_harvest_quad` | `0` | **B4** quad convergence 0/1: when the quad window opens (respawn within lead s, or freshly up <15 s), the nearest 2–3 armed+ kbots see the quad goal deflated. Lead/deflation/count are parameterized by the quad dial. |
| `k_kbot_harvest_guard` | `0` | **B4** guard stance 0/1: a non-taking kbot already in the quad zone at 280–620 qu holds a 2.5 s watch on an entry opening (east/RL entry prioritized) during the window. |
| `k_kbot_harvest_hold` | `0` | **B5** posting 0/1: a control-class kbot with RL standing in a tactical zone takes 3–6 s locked-yaw holds (timing/ambush watch). Aborts on sight/damage/water. Holds are cleared on death. |
| `k_kbot_harvest_debug` | `0` | Temporary `hvdebug` state emitter (2 s throttle, control-class only) for B4/B5 diagnosis. Lab tooling; scheduled for removal. |

## Weapon discipline (`kbot_weapons.c`)

Owner rules 2026-07-06. Seam at the top of `DesiredWeapon`, after the
quad-teammate safety check.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_weap_quadlg` | `0` | 0/1 rule 1: with quad + LG + cells and an enemy in LG reach, always shaft (validated +4.50 with quad economy 3.5/1.5). |
| `k_kbot_weap_finish` | `0` | 0/1 rule 2: after a partial RL hit on a known fresh spawn (killfeed-stamped <15 s, truth-read hp<70/armor 0, within 450 qu), finish with SNG/NG/SSG/SG — saves rockets, avoids self-damage as the enemy closes. |
| `k_kbot_weap_sgdown` | `0` | 0/1 rule 3 (`cl_weaponhide` semantics): SG carried whenever not actively firing (enemy unseen, >0.7 s since last fire, >2 s since last hurt) — the pack dropped on death then contains SG, not the real gun. No hold/camp exemptions by design. |

## The five tactical dials (`kbot_dials.c`)

Owner directive 2026-07-06. **−1 = off (byte-neutral), 0..1 = active** with
0.5 as the neutral-ish midpoint of each mapping. Every dial has a global cvar
plus four per-bot overrides `k_kbot_dial_<name>_s<slot>` (slot = kbot join
order 1–4; slot value ≥0 beats the global). All 25 default `-1`. Applications
log under KDLOG lane=dial. Sweep verdict 2026-07-06: mid values synergize
(all-0.5 best tl5 arm), extremes are toxic; tl20 confirmation of all-0.5 still
open.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_dial_engage` | `-1` | Engagement threshold. Effective aggression = dial + 0.15 × (our armed+ count − their armed+ count); steps the UTBYTE exchange decision one notch up (≥0.7) or down (≤0.3) — FINISH is never modulated — and scales the weak-stack threshold by 1.5−aggr. |
| `k_kbot_dial_hoard` | `-1` | Item hoarding vs map control: mega/armor `goal_time` × (1.2 − 0.4×dial) — high dial makes stack items cheaper (hoard) — and B5 posting cadence × (0.4 + 1.2×dial). |
| `k_kbot_dial_adhere` | `-1` | Team adherence: goals within 700 qu of a living teammate get `goal_time` × (1 − 0.3×dial), others × (1 + 0.3×dial). High = play tight (measured −9.83 at 0.8 vs frogs — spacing disproof). |
| `k_kbot_dial_quad` | `-1` | Quad commitment: parameterizes B4 — window lead = 5 + 10×dial s, goal deflation = 1 − 0.5×dial, third converger joins at ≥0.75. |
| `k_kbot_dial_share` | `-1` | Economic sharing: heavy items cost ×(1 + 0.8×dial) for the richer bot (>50 stack diff) when a poorer teammate is near/targeting the item — the poor bot gets the pickup. |
| `k_kbot_dial_<name>_s1`..`_s4` | `-1` | Per-bot override of the matching global for kbot slot 1–4 (individual personalities). −1 = inherit global. |

## Gap-jump system (`kbot_main.c`, dm3)

The learned strafe-jump plays: lanes 0/1 Ring↔Quad, 2/3 RA↔YA southern
parallels, 4 bridge→RL slot descent, 5/6 SNG/mega jumps. The full stack
(master toggle, active intent, route pricing, per-lane machinery) is default
**ON**; everything remains kbot-only. `KDLOG lane=<lane> phase=launch/land/fail`
tracks outcomes when `k_kbot_dlog` is on.

### Master toggles & lab harness

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gapjump` | `1` | Master 0/1 for the whole gap-jump frame. 0 = vanilla movement untouched. |
| `k_kbot_gj_active` | `1` | E9 ACTIVE intent 0/1: with a nav goal across a gap lane and no enemy near, deliberately drive to the lip, align, build speed and launch (instead of waiting for passive gate luck). |
| `k_kbot_gj_route` | `1` | E10 route shim 0/1: price the jump as a kbot-only travel-time edge in goal selection, and stage-steer from anywhere on the takeoff plate into the intent box. |
| `k_kbot_gj_lane` | `-1` | −1 = passive trigger (the real feature). 0..6 = trial driver: force-run one lane as an isolated landing-% harness (lab only). |
| `k_kbot_gj_probe` | `0` | 0/1 lab probe mode (movement-probe logging harness for lane calibration). |
| `k_kbot_gj_to` / `k_kbot_gj_land` | *(empty)* | `"x y z"` overrides for takeoff/landing anchors in trial mode (lab calibration). |
| `k_kbot_gj_cal` / `k_kbot_gj_caldir` / `k_kbot_gj_traj` | `0` | Calibration sweeps (heading/dir/trajectory logging) for lane seeding. Lab only. |
| `k_kbot_gj_gatelog` | `0` | 0/1 verbose `[gapjump]` gate decision log (declines, aborts, launches with speeds/offsets). |

### Launch model & commit gates (E8/E8.2)

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gj_v0` | `450` | Nominal launch speed (qu/s) for the ballistic model. |
| `k_kbot_gj_head` | `-1000` | Launch heading override (deg). Below −360 = use computed lane bearing. |
| `k_kbot_gj_head_off` | `0` | Per-launch heading offset (deg) — the "human −11°" lever. |
| `k_kbot_gj_airgain` | `0.93` | Air-acceleration discount used when computing required launch speed `v_req`. |
| `k_kbot_gj_vreq` | `0` | Direct `v_req` override (qu/s). 0 = computed per lane. |
| `k_kbot_gj_gate` | `0.98` | Required-speed gate: decline unless approach speed ≥ `v_req × gate` (stops pit-falls; E7's −9.92 came from launching slow). |
| `k_kbot_gj_launch_mul` | `1.2` | Launch floor = `v_req × mul`. Margin over bare v_req because the air-carve scrubs speed (413+ lands ~75 %, ~389 falls short). |
| `k_kbot_gj_align_tol` | `30` | Commit only when velocity heading is within this many deg of the launch (bow) heading. 0 = disabled. |
| `k_kbot_gj_maxprog` | `40` | Position gate: along-lane progress must be ≤ this (not past the lip) at commit. |
| `k_kbot_gj_ymax` | `48` | Position gate: cross-lane offset must be ≤ this at commit. |
| `k_kbot_gj_steer` | `5` | In-flight steering deadband (deg) around the target bearing before course-correcting (≤0 falls back to 5). |
| `k_kbot_gj_wp` | `0` | Pillar-gap air waypoint: bow the arc south by this many qu at mid-span (0 = straight at landing). |
| `k_kbot_gj_landrad` | `64` | Landing acceptance radius (qu) for outcome classification. |
| `k_kbot_gj_failz` | `0` | Override of the fail-Z threshold for classifying a fallen crossing. 0 = per-lane default. |
| `k_kbot_gj_timeout` | `4` | Overall play timeout (s) before abort to vanilla nav. |
| `k_kbot_gj_cool` | `0.6` | Cooldown (s) after a resolved attempt before the lane may re-trigger. |
| `k_kbot_gj_zone` | `96` | Passive trigger zone size (qu) around the takeoff anchor. |
| `k_kbot_gj_runup` | `0` | Extra straight run-up distance (qu) in trial mode. |
| `k_kbot_gj_build` | `0` | E8 experimental circle-jump run-up 0/1 in the passive path (build to v_req before launch). The active path has its own builder (`app_build`). |
| `k_kbot_gj_buildtime` | `1.5` | Max circle-build duration (s). |
| `k_kbot_gj_build_angle` | `42` | Circle-build carve angle (deg). Also the default for `appcarve_angle`. |

### Active approach (E9)

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gj_intent_back` | `384` | Intent region: engage when within this many qu BEHIND the lip along the lane. |
| `k_kbot_gj_intent_perp` | `176` | Intent region: max lateral distance (qu) from the lane line. |
| `k_kbot_gj_intent_zband` | `56` | Intent region height band (qu) — keeps intent on the takeoff ledge. |
| `k_kbot_gj_apptime` | `3.5` | Approach abort-to-vanilla timeout (s). Lanes 5/6 stalled timeouts also get a 6 s re-engage suppression (E14). |
| `k_kbot_gj_lookahead` | `112` | Drive steering lookahead point (qu) on the launch ray. |
| `k_kbot_gj_launch_win` | `48` | Launch when within this along-lane window of the lip... |
| `k_kbot_gj_launch_perp` | `28` | ...and within this lateral offset, and fast enough. |
| `k_kbot_gj_app_build` | `1` | 0/1: allow circle-acceleration toward the lip when under v_req. |
| `k_kbot_gj_app_cool` | `0.3` | Re-engage cooldown (s) after a decline — breaks per-frame flicker. |
| `k_kbot_gj_app_align` | `45` | Active launch alignment tolerance (deg) vs the corridor axis. |
| `k_kbot_gj_north_max` | `12` | Reject launches more than this many qu north of the corridor line (central-pillar clip protection, lanes 0/1). |
| `k_kbot_gj_south_bias` | `24` | Bias the approach drive this many qu south into the open corridor. |
| `k_kbot_gj_edge_time` | `1.3` | Route shim: priced cost (s) of the hop edge in goal selection (measured 0.65 s flight + speed build). |
| `k_kbot_gj_route_back` | `512` | Route shim takeoff-side region: how far behind the lip the route is offered. |
| `k_kbot_gj_route_lat` | `352` | Route shim region: max lateral distance from the lane. |

### Mirror lanes 2/3, RA↔YA (E10c/E11/E13)

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gj_mirrorcarve` | `1` | 0/1: relocate lanes 2/3 to the diagonal lip-to-lip chord + north launch-aim so the parallel jump commits in-match. Lanes 0/1 untouched. |
| `k_kbot_gj_mcarve_mul` | `1.20` | Mirror-lane launch floor multiplier (A/B-validated). |
| `k_kbot_gj_mcarve_bow` | `40` | Mirror-lane air-arc bow (qu). |
| `k_kbot_gj_appcarve` | `0` | OPTION-2 approach-carve 0/1 (REFUTED in A/B — keep 0): keep building past the launch floor in the runway. |
| `k_kbot_gj_appcarve_target` | `1.06` | Approach-carve build ceiling = target × floor. |
| `k_kbot_gj_appcarve_angle` | `0` | Approach-carve angle (deg). 0 = use `build_angle`. |
| `k_kbot_gj_mchain` | `1` | E13 chain-hop 0/1 for lanes 2/3: straight-line hop into the launch box (fixes the line option-2 broke; tops speed into the 420–491 LAND band). |
| `k_kbot_gj_mchain_min` | `320` | Min grounded speed for the mirror chain-hop. |
| `k_kbot_gj_mchain_tgt` | `450` | Chain-hop target speed. |
| `k_kbot_gj_mchain_exit` | `380` | Min predicted exit speed to accept the hop. |
| `k_kbot_gj_mchain_back` | `240` | How far behind the launch box the chain-hop may start. |

### RL slot descent, lane 4 (E12)

Bridge → RL through the pent-yard firing slot: a 16 qu origin-z needle at the
wall, so the commit needs a speed **window**, not just a floor.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gj_rl` | `1` | 0/1: lane 4 exists. |
| `k_kbot_gj_rl_mul` | `0.99` | Speed-window floor = mul × lane v_req. |
| `k_kbot_gj_rl_max` | `1.09` | Speed-window ceiling = max × lane v_req (too hot overshoots the slot). |
| `k_kbot_gj_rl_win` | `24` | Along-lip commit window (qu). |
| `k_kbot_gj_rlwp` | *(empty)* | `"x y z"` live override of the lane-4 air waypoint. |
| `k_kbot_gj_wp_lead` | `48` | How early (qu before the waypoint) the air-carve switches to the landing. |
| `k_kbot_gj_airseat` | `1` | 0/1 mid-air seating correction for the slot entry. |
| `k_kbot_gj_rl_bangle` | `60` | Lane-4 build/carve angle (deg). |
| `k_kbot_gj_rl_unseen` | `1` | Owner rule 2026-07-05, 0/1: only attempt the RL jump while NO enemy has line of sight to the bot (the setup is combat-defenseless). |
| `k_kbot_gj_chain` | `1` | E12b chain-hop 0/1: one carve hop tops the ground build (~450 cap) into the slot speed window (~459–506). |
| `k_kbot_gj_chain_min` | `410` | Min grounded speed for the chain-hop to be able to reach the floor. |
| `k_kbot_gj_chain_tol` | `20` | Max shortfall (qu) of the predicted touchdown vs the nominal launch origin. |

### SNG/mega jumps, lanes 5/6 (E14, ticket #24)

Yard → 120-step across the void band, and west ledge → mega perch.

| Cvar | Default | Description |
|---|---|---|
| `k_kbot_gj_sng` | `1` | 0/1: lanes 5/6 exist. |
| `k_kbot_gj_sng_mul` | `0` | Launch speed-floor multiplier for lanes 5/6, gated on the ALONG-axis velocity component. 0 = per-lane defaults (lane 5: 1.0, lane 6: 1.08). |
| `k_kbot_gj_schain` | `1` | 0/1: lane-5 scripted L-approach (corridor mouth → south dash along the demo-measured x≈−540 corridor), incl. its 8 s apptime and pre-route decline skip. |

---

## Quick recipes

Full komodobot tournament setup (UTBYTE + HARVEST + weapon discipline, the
validated 2026-07-06 configuration; gap-jump stack is already on by default):

```
set k_kbot_model 3
set k_kbot_commit 1.5
set k_kbot_harvest_route 2.5
set k_kbot_harvest_threat 2.0
set k_kbot_harvest_anchor 1.4
set k_kbot_harvest_quad 1
set k_kbot_harvest_guard 1
set k_kbot_harvest_hold 1
set k_kbot_weap_quadlg 1
set k_kbot_weap_finish 1
set k_kbot_weap_sgdown 1
```

Add the dial working point (tl5-validated; tl20 confirmation still open):

```
set k_kbot_dial_engage 0.5
set k_kbot_dial_hoard 0.5
set k_kbot_dial_adhere 0.5
set k_kbot_dial_quad 0.5
set k_kbot_dial_share 0.5
```

Lab diagnosis: `set k_kbot_dlog 1` (+ `k_kbot_gj_gatelog 1` /
`k_kbot_harvest_debug 1` for the verbose per-mechanism traces).
