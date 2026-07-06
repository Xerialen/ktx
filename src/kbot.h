/*
 kbot.h -- KomodoBrain (WP3.3: engage/disengage discipline)

 Brain seam: bots flagged as "komodobots" have their think routed through
 KBot_Frame(), which delegates movement/nav/combat 100% to the stock frogbot
 logic (the WP3.2 null experiment measured pure delegation at control parity,
 -5.6, while the weave cost 25-30 frags/game -- so vanilla actuation stays).

 WP3.3 adds fight DISCIPLINE at the decision level only: a weak kbot
 (disarmed or low stack, see KBot_AvoidFights) never HUNTS -- its positive
 enemy-goal desire is cleared in UpdateGoal, so the vanilla item economy
 sends it collecting instead of re-engaging. Vanilla's own repel machinery
 (negative desire + enemy hunting us) still biases goals away from threats,
 and combat micro (aim, dodge, look_object) is untouched: fights that find
 the bot are still fought with full vanilla skill.

 Expects g_local.h to have been included first (KTX header convention).
 */
#ifndef KTX_KBOT_H
#define KTX_KBOT_H

#ifdef BOT_SUPPORT

#define KBOT_VERSION "kbot-0.23.0-routepolicy"

// States for fb.kbot
#define KBOT_STATE_OFF     0	// stock frogbot (default; fb is memset to 0)
#define KBOT_STATE_MARKED  1	// komodobot, identity not yet logged this match
#define KBOT_STATE_ACTIVE  2	// komodobot, one-time frame log emitted

// Flag a freshly added bot as a komodobot: sets fb.kbot, stamps identity
// markers (userinfo "kbot" key + "kb:" name prefix) and logs to server console.
// Owner-rostered seat names (k_kbot_name1..4) skip the name prefix.
void KBot_MarkBot(gedict_t *bot);

// True when `name` matches an effective roster seat name (k_kbot_name1..4
// cvar or the built-in hib/dag/Angua/Rock defaults).
qbool KBot_IsRosterSeatName(const char *name);

// Effective roster name for seat n (1..4): cvar when set, owner default
// otherwise. Fills buf and returns it; NULL for n out of range.
const char* KbotRosterName(int n, char *buf, int bufsize);

// Per-frame brain entry point, called from BotsThinkTime() for flagged bots.
// Returns true if the brain fully handled this frame's think (BotsThinkTime
// then returns immediately). WP2.1: always returns false -- pure delegation
// to the stock frogbot code path.
qbool KBot_Frame(gedict_t *self);

// Fight discipline predicate (WP3.3): true when this kbot should decline to
// hunt -- disarmed (no RL with rockets / no LG with cells) or low stack
// (health + armor below KBOT_WEAK_STACK). Fresh spawns are "disarmed" on
// weapon-stripped maps, which is the post-death discipline: collect first,
// re-engage once armed. Decision-level consumers only; never movement.
qbool KBot_AvoidFights(gedict_t *self);

// ---- E6: gap-crossing strafe-jump play ----
// Final-authority movement override for a triggered gap crossing on dm3
// (Ring<->Quad, RA<->YA lanes, both directions). Reuses the E1 c=0 air-carve
// as the air-control engine. Fully inert (returns false, vanilla movement
// stands) unless k_kbot_gapjump != 0 AND (a trial lane is selected via
// k_kbot_gj_lane, OR the passive trigger fires with no enemy near). Called
// from the bot_movement.c dir_move_ seam after BotApplyMoveProbe.
qbool KBot_GapjumpFrame(gedict_t *self, qbool *jumping, qbool *firing,
					   int *impulse, vec3_t direction);

// True while the gap-jump state machine owns this bot's movement/view
// (any state but idle). kbot_threataim yields on it.
qbool KBot_GapjumpBusy(gedict_t *self);

// ---- kbot_threataim (issue #29): clear-state crosshair placement ----
// Milton-derived threat-point pre-aim for humanmode kbots. Master cvar
// k_kbot_threataim: 0 off (bit-identical), 1 static table, 2 +hm-belief
// weighting, 3 +hold/snap scheduler with staleness and rear checks.
// Map data is generated (komodobots2 tools/gen_threataim_table.py); dm3 only.

// Once per map, next to HMode_MapInit(): reset per-bot state, key threat
// cells to their nearest tracked major item edicts (fixed map facts).
void KBot_ThreatAimMapInit(void);

// Per-frame, from BotSetCommand BEFORE the desired_angle makevectors: in the
// CLEAR state (no visible enemy, no view-owning movement/combat logic) owns
// fb.desired_angle; otherwise does nothing. Also emits the [ta-contact]
// first-sight KPI line on the enemy_visible rising edge (k_hm_debug).
void KBot_ThreatAimFrame(gedict_t *self);

// ---- kbot_routepolicy: Milton-derived route policy (dm3 only) ----
// Master cvar k_kbot_routepolicy: 0 off (bit-identical), 1 transition bias in
// EvalGoal, 2 +spawn openings. Decision-level goal-desire bias only; movement,
// routing and the gap-jump play are untouched. Table is generated
// (komodobots experiments/route_policy/gen_routepolicy_table.py); dm3 only.

// Once per map, next to KBot_ThreatAimMapInit(): reset per-bot state, bind
// node ids to their live item edicts (fail-safe inert if any is missing).
void KBot_RoutePolicyMapInit(void);

// From BotClientEntersEvent: reset the visit chain; tier 2 samples the
// spawn-conditioned opening resource.
void KBot_RoutePolicySpawnEvent(gedict_t *self, gedict_t *spawn_pos);

// Per-frame from KBot_Frame: proximity visit tracking + [kb-route] telemetry.
void KBot_RoutePolicyTrack(gedict_t *self);

// EvalGoal consumer (after HMode_GoalDesireBias): scales a kbot's desire for
// a route resource by P(next | last visited); identity when off/not a node.
float KBot_RoutePolicyDesireBias(gedict_t *self, gedict_t *goal_entity, float desire);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
