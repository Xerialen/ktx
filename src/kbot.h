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

 WP3.4 adds the PREDATOR WEAVE (owner-directed): the mode-23 nav-weave
 re-enters as an attack/chase-only mode -- engaged solely when hunting a
 visible enemy who is not facing us (or every facing enemy is significantly
 weaker), with instant drop / 1 s re-arm hysteresis. Rotation weave remains
 off (measured -25..-30 frags/game).

 Expects g_local.h to have been included first (KTX header convention).
 */
#ifndef KTX_KBOT_H
#define KTX_KBOT_H

#ifdef BOT_SUPPORT

#define KBOT_VERSION "kbot-0.6.0-predatorweave"

// States for fb.kbot
#define KBOT_STATE_OFF     0	// stock frogbot (default; fb is memset to 0)
#define KBOT_STATE_MARKED  1	// komodobot, identity not yet logged this match
#define KBOT_STATE_ACTIVE  2	// komodobot, one-time frame log emitted

// Flag a freshly added bot as a komodobot: sets fb.kbot, stamps identity
// markers (userinfo "kbot" key + "kb:" name prefix) and logs to server console.
void KBot_MarkBot(gedict_t *bot);

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

// Predator weave (WP3.4, owner-directed): the mode-23 weave as an attack/
// chase mode ONLY -- engaged when the bot's nav goal is a visible enemy and
// no visible facing enemy is a real threat (full rule in kbot_main.c).
// Called once per frame from the moveprobe dispatch; carries asymmetric
// hysteresis (instant drop, 1 s continuous re-arm). Rotation weave stays off.
qbool KBot_PredatorWeave(gedict_t *self);

// Side-effect-free query of the current predator-weave engage state
// (telemetry mirror only).
qbool KBot_PredatorWeaveActive(gedict_t *self);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
