# KomodoBrain (ktx)

The server-side bot AI this fork adds on top of stock KTX Frogbot, on the
**canonical `master` branch**. It is the **kbot** family — six `kbot_*.c`
modules (5977 LOC) wired into frogbot through a cvar-gated seam. This glossary
covers the **code**: how the layer is wired and what each module does. The
gameplay/measurement glossary lives in KomodoBench's `CONTEXT.md`; shared nouns
(kbot, KomodoBrain, Frogbot, gapjump, lane, Milton, KDLOG, actuation) keep the
same meaning there.

The **humanmode** family (`hm.c`, comms + teamplay) is NOT on master — it lives
on the experimental `mm2humanmode` branch. So on canonical master the
**Communication** behaveval category has no implementation, and Teamplay is only
partial (via dials). Each module/term below is tagged with the behaveval
category it drives.

## Language

### The seam

**kbot flag** (`fb.kbot`):
The per-bot marker (set by `KBot_MarkBot`, `bot_commands.c`) that routes a bot
into KomodoBrain. A bot without it runs pure stock Frogbot.

**seam**:
The edit points where stock `bot_*.c` calls into `KBot_*` — top entry
`KBot_Frame` (`bot_botthink.c`), then per-feature inject points (harvest anchor
/ quad in `bot_botgoals.c`, harvest hold + gapjump in `bot_movement.c`, route
penalties in `bot_routing.c`). Each returns a vanilla-equivalent value when the
bot isn't a kbot or the feature's cvar is 0, so the seam is inert by default.

**cvar-gate**:
Every feature has a `k_kbot_*` cvar defaulting to 0 = bit-identical vanilla.
Turning a feature on is opt-in per bench run. Cvars are read dynamically, no
static table.
_Avoid_: feature flag

### Modules

**kbot_main** (3506 LOC):
The skeleton — `KBot_Frame` / `KBot_MarkBot`, the **discipline** veto, the
**tunables**, and the entire **GJ trick-jump engine**.
_Category_: Combat IQ (discipline) + Mechanics (trick jumps)

**harvest** (`kbot_harvest.c`, 968, `k_kbot_harvest_*`):
The item-economy / map-control system — **the module that produced the best
frag results.** Built around the **carried-value scalar V**: value-aware route
discipline (water/threat path penalties), zone **anchoring** (a stacked bot
lives in its RA zone), position **hold**/guard, and quad-window commitment.
Monetises map control, which is the top win predictor.
_Category_: Rotation and Control (+ Combat IQ risk)

**dials** (`kbot_dials.c`, 398, `k_kbot_dial_*`):
Five high-level behavior-appetite knobs (the "rattar"): **engage** (attack/flee
appetite from own stack), **hoard** (heavy-item timing vs holding), **adhere**
(compact crossfire vs lone), **quad** (quad-window commitment), **share** (yield
RA/mega to a needier teammate).
_Category_: cross-category — engage→Combat IQ, share/adhere→Teamplay, hoard/quad→Rotation

**models** (`kbot_models.c`, 523, `k_kbot_model*`):
Shared vocabulary the other modules read — stack classes (`KBM_`), goal
categories (`KBC_`), and the team model (blue/red). Foundation, not a behavior.
_Category_: cross-cutting

**weapons** (`kbot_weapons.c`, 167, `k_kbot_weap_*`):
Weapon-selection logic — finish choice, quad+LG, SG down-switch.
_Category_: Mechanics / Combat IQ

**dlog** (`kbot_dlog.c`, 415, `k_kbot_dlog`):
The decision-trace logger — writes **KDLOG** to `server.log`. The instrument
behaveval's bench-only questions read.
_Category_: instrumentation

### Key concepts

**discipline**:
The veto that zeros a kbot's hunt-desire when it's under-stacked (weak), sending
it to collect instead of fight. The one decision-layer lever that paid — only
*negative* fight selection helps; enemy-seeking loses. Lives in `kbot_main`.
_Category_: Combat IQ

**carried-value (V)**:
A 0..1 scalar = normalized stack × firepower — how much a bot is carrying and
therefore worth protecting. Every HARVEST mechanism reads it: a stacked carrier
anchors/holds, an empty bot roams to collect.

**trick jump**:
A discrete, scripted traversal jump across a dm3 gap — the gapjump lanes
(ring↔quad, RA↔YA mirror, bridge→RL needle), executed by the `GJ_*` engine in
`kbot_main`. **ACTIVE and kept**, reverse-engineered from human MVDs. The spoken
name for what the code calls a gapjump/lane.
_Category_: Mechanics

**bunnyhop**:
Continuous strafe-jump speed as a movement *mode*. **PAUSED** — Lessons 6/8:
naive bunny and the carve-motor couldn't be monetised in dm3 4on4. Distinct from
a trick jump, which is discrete and active. Parked on `feat/kbot-*` branches.
_Avoid_: constant bunnyjumping

**tunables**:
The full `k_kbot_*` parameter surface (thresholds, biases, weights) — the search
space bench tuning moves in. The **dials** are the five named high-level ones.
