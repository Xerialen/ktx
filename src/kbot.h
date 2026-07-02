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

#define KBOT_VERSION "kbot-0.13.0-bunny"

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

// ---- Bunny travel (WP4.0, kbot_main.c, owner track B) ----
// Per-frame gate routing a kbot through the in-tree mode-23 air-strafe
// actuation for TRAVEL only: no visible enemy, straight route segment
// (k_kbot_bunny_maxturn), corridor clearance, damage cooldown
// (k_kbot_bunny_cooldown). k_kbot_bunny 0 = byte-identical vanilla.
qbool KBot_BunnyTravel(gedict_t *self);
// Side-effect-free engage-state query (telemetry mirror only).
qbool KBot_BunnyActive(gedict_t *self);
// Damage-GIVEN stamp for the bunny cooldown (kbot attackers only).
void KBot_NoteDamageGiven(gedict_t *attacker);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
