/*
 kbot.h -- KomodoBrain (WP2.2: navigation + movement)

 Brain seam: bots flagged as "komodobots" have their think routed through
 KBot_Frame(), which still delegates nav/goals/combat 100% to the stock
 frogbot logic. Since WP2.2 their ACTUATION differs: BotApplyMoveProbe()
 (bot_movement.c) routes kbot-flagged bots into the mode-23 nav-weave by
 default (frogbot navigation decides WHERE, the cs->0 air-accel bunnyhop
 weave decides HOW FAST) plus an auto-armed circle-jump launch when parked.
 Baseline frogbots are untouched (every kbot branch is fb.kbot-guarded).

 Expects g_local.h to have been included first (KTX header convention).
 */
#ifndef KTX_KBOT_H
#define KTX_KBOT_H

#ifdef BOT_SUPPORT

#define KBOT_VERSION "kbot-0.2.0-weave"

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

#endif // BOT_SUPPORT

#endif // KTX_KBOT_H
