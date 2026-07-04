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

#define KBOT_VERSION "kbot-0.20.1-mirrorlane"

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

// ---- E6: gap-crossing strafe-jump play ----
// Final-authority movement override for a triggered gap crossing on dm3
// (Ring<->Quad, RA<->YA lanes, both directions). Reuses the E1 c=0 air-carve
// as the air-control engine. Fully inert (returns false, vanilla movement
// stands) unless k_kbot_gapjump != 0 AND (a trial lane is selected via
// k_kbot_gj_lane, OR the passive trigger fires with no enemy near). Called
// from the bot_movement.c dir_move_ seam after BotApplyMoveProbe.
qbool KBot_GapjumpFrame(gedict_t *self, qbool *jumping, qbool *firing,
					   int *impulse, vec3_t direction);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
