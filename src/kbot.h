/*
 kbot.h -- KomodoBrain (WP3.1: navigation + movement + item economy)

 Brain seam: bots flagged as "komodobots" have their think routed through
 KBot_Frame(), which still delegates nav/goals/combat 100% to the stock
 frogbot logic. Since WP2.2 their ACTUATION differs: BotApplyMoveProbe()
 (bot_movement.c) routes kbot-flagged bots into the mode-23 nav-weave by
 default (frogbot navigation decides WHERE, the cs->0 air-accel bunnyhop
 weave decides HOW FAST) plus an auto-armed circle-jump launch when parked.
 Since WP2.2b the weave is COMBAT-GATED: it only drives out-of-combat
 traversal; while fb.look_object is a player (frogbot movement's own
 dodge predicate, +1.5 s hysteresis) movement delegates 100% to vanilla
 frogbot combat behavior and the auto-launch is disarmed.
 Since WP3.1 GOAL SELECTION is predictive (kbot_goals.c): respawn-timer-
 aware scoring of the majors, delegating to the vanilla economy whenever
 nothing scores a committed rotation.
 Baseline frogbots are untouched (every kbot branch is fb.kbot-guarded).

 Expects g_local.h to have been included first (KTX header convention).
 */
#ifndef KTX_KBOT_H
#define KTX_KBOT_H

#ifdef BOT_SUPPORT

#define KBOT_VERSION "kbot-0.4.1-items"

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

// Predictive item-economy goal selection (WP3.1, kbot_goals.c). Called from
// UpdateGoal() for kbot-flagged bots, after the lab fixed_goal pin. Returns
// true when it committed a goal (having set the same state UpdateGoal's tail
// sets); false delegates to the vanilla goal economy untouched.
qbool KBot_SelectGoal(gedict_t *self);

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
