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

#define KBOT_VERSION "kbot-0.25.1-arena"

// ---- tournament decision models (kbot_models.c, 2026-07-06) ----
// Three cvar-gated models over the frogbot value function, data-filled from
// the Book/]sr[ dm3 win corpus (komodobots2 report 2026-07-06):
//   k_kbot_model 1=TDM 2=KAPTEN 3=UTBYTE (0 off, byte-neutral);
//   k_kbot_model_red / k_kbot_model_blue override per team for face-offs.
int KBot_StackClass(gedict_t *p);       // 0 spawn / 1 mid / 2 armed / 3 control
int KBot_TeamRLLG(gedict_t *self);      // RL/LG count in team hands (owner param)
int KBot_ActiveModel(gedict_t *self);
float KBot_ModelScaleGoal(gedict_t *self, gedict_t *goal, float desire);
float KBot_ModelScaleHunt(gedict_t *self, gedict_t *en, float desire);

// ---- KDLOG decision-log emitter (kbot_dlog.c) ----
// Structured tactical-decision telemetry (G_cprint "KDLOG ..." lines into
// server.log), gated by cvar k_kbot_dlog (0 off / 1 kbots / 2 all bots) and
// match-in-progress. Strictly read-only. Consumed by mvd_analyzer
// -decision-log. Design: komodobots2 docs/specs/2026-07-05-decision-log-design.md.
void KDLog_MarkTrigger(gedict_t *plr, const char *trig); // why the next goal refresh fires
void KDLog_GoalReset(gedict_t *self);                    // arm collector (UpdateGoal start)
void KDLog_GoalCandidate(gedict_t *self, gedict_t *goal, float desire, float goal_time);
void KDLog_GoalChosen(gedict_t *self);                   // emit goal record (UpdateGoal end)
void KDLog_Enemy(gedict_t *self);                        // emit on enemy-target change
void KDLog_Evade(gedict_t *self);                        // emit on bot_evade flip
void KDLog_Play(gedict_t *self, const char *lane, const char *phase, const char *detail);

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
qbool KBot_RouteFocusIgnore(gedict_t *self, gedict_t *enemy);

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
