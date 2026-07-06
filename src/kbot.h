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

#define KBOT_VERSION "kbot-0.28.0-dials"

// ---- tournament decision models (kbot_models.c, 2026-07-06) ----
// Three cvar-gated models over the frogbot value function, data-filled from
// the Book/]sr[ dm3 win corpus (komodobots2 report 2026-07-06):
//   k_kbot_model 1=TDM 2=KAPTEN 3=UTBYTE (0 off, byte-neutral);
//   k_kbot_model_red / k_kbot_model_blue override per team for face-offs.
// stack classes (KBot_StackClass) -- shared vocabulary with the analyzer
#define KBM_SPAWN   0
#define KBM_MID     1
#define KBM_ARMED   2
#define KBM_CONTROL 3

// goal categories (KBot_GoalCategory) -- 1:1 with the analyzer item kinds
#define KBC_ARMOR   0
#define KBC_MEGA    1
#define KBC_HEALTH  2
#define KBC_WBIG    3
#define KBC_WSMALL  4
#define KBC_AMMO    5
#define KBC_POWERUP 6
#define KBC_PACK    7
#define KBC_OTHER   8

int KBot_StackClass(gedict_t *p);       // 0 spawn / 1 mid / 2 armed / 3 control
int KBot_GoalCategory(gedict_t *g);     // KBC_* for a world goal entity
int KBot_TeamRLLG(gedict_t *self);      // RL/LG count in team hands (owner param)
int KBot_ActiveModel(gedict_t *self);
float KBot_ModelScaleGoal(gedict_t *self, gedict_t *goal, float desire);
float KBot_ModelScaleHunt(gedict_t *self, gedict_t *en, float desire);

// ---- HARVEST possession layer (kbot_harvest.c, 2026-07-06) ----
// Retention on top of UTBYTE: cost shaping only (R2 -- never desire), one
// cvar per mechanism, byte-neutral defaults. Spec: komodobots2
// docs/specs/2026-07-06-harvest-model-design.md.
float KBot_CarriedValue(gedict_t *p);        // 0..1, stack x firepower
float KBot_HarvestWaterPenalty(gedict_t *p); // B1: EvalPath water-marker cost
float KBot_HarvestThreatPenalty(gedict_t *self, gedict_t *m); // B2: place threat
void KBot_HarvestDeathEvent(gedict_t *targ); // B2: death-memory feed (Killed)
float KBot_HarvestAnchorShim(gedict_t *self, gedict_t *goal, float goal_time); // B3
float KBot_HarvestQuadShim(gedict_t *self, gedict_t *goal, float goal_time);   // B4
qbool KBot_HarvestHoldFrame(gedict_t *self, qbool *jumping, vec3_t direction); // B4/B5
qbool KBot_HarvestHolding(gedict_t *p);      // true while a B4/B5 hold owns the bot

// ---- weapon discipline (kbot_weapons.c, owner rules 2026-07-06) ----
// k_kbot_weap_quadlg / _finish / _sgdown, defaults 0 byte-neutral.
int KBot_WeaponOverride(gedict_t *self);     // IT_* or 0 (vanilla selection)
void KBot_WeaponsDeathEvent(gedict_t *targ); // killfeed stamps for rule 2

// enemy-state estimator (kbot_harvest.c) -- THE human_mode swap point
int KBot_EnemyClassEst(gedict_t *self, gedict_t *en);

// ---- the five tactical dials (kbot_dials.c, owner directive 2026-07-06) ----
// k_kbot_dial_<engage|hoard|adhere|quad|share> global + _s<1..4> per-bot
// override (fb.kbot_slot). Default -1 = off (byte-neutral), active 0..1.
// KDLOG lane=dial on every outcome-changing application.
#define KDIAL_ENGAGE 0
#define KDIAL_HOARD  1
#define KDIAL_ADHERE 2
#define KDIAL_QUAD   3
#define KDIAL_SHARE  4
float KBot_Dial(gedict_t *self, int dial);                    // resolved, -1 off
float KBot_DialEngageAggr(gedict_t *self);                    // dial + team balance
float KBot_DialWeakScale(gedict_t *self);                     // retreat threshold x
int KBot_DialEngageAdjust(gedict_t *self, gedict_t *en, int decision); // KBX step
float KBot_DialGoalShim(gedict_t *self, gedict_t *goal, float goal_time); // D2/D3/D5
float KBot_DialHoldScale(gedict_t *self);                     // B5 cadence x
float KBot_DialQuadLead(gedict_t *self);                      // B4 lead seconds
float KBot_DialQuadDeflate(gedict_t *self);                   // B4 goal_time x
int KBot_DialQuadCount(gedict_t *self);                       // B4 convergers

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
