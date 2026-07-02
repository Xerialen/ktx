/*
 kbot_main.c -- KomodoBrain skeleton (WP2.1)

 Proves the brain seam: KBot_MarkBot() flags a bot as a komodobot and stamps
 its identity into run evidence; KBot_Frame() is the per-frame entry point,
 which in this WP delegates 100% to the stock frogbot logic.
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"

void KBot_MarkBot(gedict_t *bot)
{
	char newname[CLIENT_NAME_LEN];
	char infobuf[64];
	int entity;

	if (!bot || !bot->isBot)
	{
		return;
	}

	entity = NUM_FOR_EDICT(bot);
	bot->fb.kbot = KBOT_STATE_MARKED;

	// Identity markers: userinfo key + "kb:" name prefix, so identity shows
	// up in ktxstats / MVD player names.
	trap_SetBotUserInfo(entity, "kbot", KBOT_VERSION, 0);
	if (strncmp(bot->netname, "kb:", 3))
	{
		snprintf(newname, sizeof(newname), "kb:%s", bot->netname);
		trap_SetBotUserInfo(entity, "name", newname, 0);
		infokey(bot, "name", bot->netname, CLIENT_NAME_LEN); // refresh game-side copy
	}

	// Advertise the brain version via serverinfo (set once, on first kbot).
	infokey(world, "kbot_version", infobuf, sizeof(infobuf));
	if (strnull(infobuf))
	{
		localcmd("serverinfo kbot_version %s\n", KBOT_VERSION);
	}

	G_cprint("[kbot] slot=%d name=%s version=%s\n", entity, bot->netname, KBOT_VERSION);
}

// Per-frame brain entry point. WP2.1: pure delegation -- log identity once,
// then return false so BotsThinkTime() runs the stock frogbot logic unchanged.
qbool KBot_Frame(gedict_t *self)
{
	if (self->fb.kbot == KBOT_STATE_MARKED)
	{
		self->fb.kbot = KBOT_STATE_ACTIVE;
		G_cprint("[kbot] frame active slot=%d name=%s version=%s time=%f\n",
					NUM_FOR_EDICT(self), self->netname, KBOT_VERSION,
					g_globalvars.time);
	}

	return false; // not handled: fall through to stock frogbot think
}

// ---- WP3.3: engage/disengage discipline ----

// "Armed" = a real duel weapon with ammo to use it. Thresholds follow the
// codebase's own conventions (AttackRespawns treats RL viable at rockets > 3).
#define KBOT_ARMED_ROCKETS  3	// rockets required to count RL as armed
#define KBOT_ARMED_CELLS    15	// cells required to count LG as armed
#define KBOT_WEAK_STACK     70	// health + armor below this = too weak to hunt

// True when this kbot should decline to HUNT (goal-level only): it has no
// usable duel weapon, or its stack is critically low. Fresh spawns on
// weapon-stripped maps are disarmed by definition -- this is the post-death
// discipline: collect armor/weapon first, re-engage once armed. Deliberately
// side-effect free and marker-free (reads only self->s.v scalars).
qbool KBot_AvoidFights(gedict_t *self)
{
	int held = (int)self->s.v.items;
	qbool armed = ((held & IT_ROCKET_LAUNCHER) && (self->s.v.ammo_rockets > KBOT_ARMED_ROCKETS))
			|| ((held & IT_LIGHTNING) && (self->s.v.ammo_cells > KBOT_ARMED_CELLS));

	if (!armed)
	{
		return true;
	}
	if ((self->s.v.health + self->s.v.armorvalue) < KBOT_WEAK_STACK)
	{
		return true;
	}

	return false;
}

// ---- WP3.4: predator weave (owner-directed experiment) ----
//
// Rule: the bot may only weave TOWARD an enemy that is not facing it, unless
// every facing enemy is significantly weaker. The weave (mode-23 actuation)
// is exclusively an attack/chase mode -- rotation weave stays OFF (measured
// -25..-30 frags/game in real matches).
//
// Provenance: facing and stack checks read server-side-omniscient state
// (enemy view yaw, exact health/armor/ammo), like parts of the vanilla
// frogbot economy. Acceptable for the lab bench per PRD; flagged for any
// future public-server readiness review.

#define KBOT_PRED_FACING_CONE   60.0f	// deg half-angle: enemy view yaw vs bearing to us
#define KBOT_PRED_FACING_RANGE  1500.0f	// qu: facing only matters inside engagement range
#define KBOT_PRED_WEAK_MARGIN   50.0f	// their (health+armor) < ours - margin = weaker
#define KBOT_PRED_REARM_SECS    1.0f	// condition must hold this long to (re)engage

static float kbot_pred_ok_since[MAX_CLIENTS];	// start of current continuous-true window
static int kbot_pred_active[MAX_CLIENTS];		// weave currently engaged

static qbool KBot_Armed(gedict_t *p)
{
	int held = (int)p->s.v.items;

	return ((held & IT_ROCKET_LAUNCHER) && (p->s.v.ammo_rockets > KBOT_ARMED_ROCKETS))
			|| ((held & IT_LIGHTNING) && (p->s.v.ammo_cells > KBOT_ARMED_CELLS));
}

// Enemy p is "facing" self: self within +/-KBOT_PRED_FACING_CONE of p's view
// yaw AND inside engagement range. Degenerate zero-distance counts as facing
// (conservative).
static qbool KBot_EnemyFacingMe(gedict_t *p, gedict_t *self)
{
	vec3_t d;
	float yaw_to_me, delta;

	VectorSubtract(self->s.v.origin, p->s.v.origin, d);
	if ((d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
		> (KBOT_PRED_FACING_RANGE * KBOT_PRED_FACING_RANGE))
	{
		return false;
	}
	d[2] = 0;
	if (VectorNormalize(d) <= 0)
	{
		return true;
	}
	yaw_to_me = vectoyaw(d);
	delta = yaw_to_me - p->s.v.v_angle[YAW];
	while (delta > 180.0f) delta -= 360.0f;
	while (delta < -180.0f) delta += 360.0f;

	return (delta >= -KBOT_PRED_FACING_CONE) && (delta <= KBOT_PRED_FACING_CONE);
}

// p is significantly weaker than self: lacks a usable duel weapon while we
// are armed, or their stack trails ours by more than the margin.
static qbool KBot_SignificantlyWeaker(gedict_t *p, gedict_t *self)
{
	if (!KBot_Armed(p) && KBot_Armed(self))
	{
		return true;
	}

	return (p->s.v.health + p->s.v.armorvalue)
			< (self->s.v.health + self->s.v.armorvalue - KBOT_PRED_WEAK_MARGIN);
}

// The raw per-frame predator condition (no hysteresis). All three rules:
// 1. we have a visible target enemy AND our nav goal IS that enemy (so the
//    marker-path weave provably heads toward them -- the nav-carrot variant);
// 2. no visible enemy inside the cone+range is facing us unless significantly
//    weaker (checked over ALL visible enemies, not just the target);
// 3. the WP3.3 discipline predicate does not say avoid-fights.
static qbool KBot_PredatorConditions(gedict_t *self)
{
	gedict_t *target = NULL;
	gedict_t *p;

	if (ISDEAD(self) || KBot_AvoidFights(self))
	{
		return false;
	}

	if (self->fb.look_object && (self->fb.look_object->ct == ctPlayer))
	{
		target = self->fb.look_object;
	}
	else if ((self->s.v.goalentity > 0) && (self->s.v.goalentity < MAX_EDICTS)
			 && (g_edicts[self->s.v.goalentity].ct == ctPlayer))
	{
		target = &g_edicts[self->s.v.goalentity];
	}
	if (!target || ISDEAD(target) || SameTeam(self, target))
	{
		return false;
	}
	if (!Visible_360(self, target))
	{
		return false;
	}
	// Weave direction must be TOWARD the enemy: the mode-23 weave follows the
	// marker path to s.v.goalentity, so require the goal to BE the target.
	if (self->s.v.goalentity != NUM_FOR_EDICT(target))
	{
		return false;
	}

	// Rule 2 over all visible enemies (4on4: never weave into a side-LG).
	// Check order is cheap -> expensive: cone math, stack math, traceline.
	for (p = world; (p = find_plr(p));)
	{
		if ((p == self) || ISDEAD(p) || SameTeam(self, p))
		{
			continue;
		}
		if (!KBot_EnemyFacingMe(p, self))
		{
			continue;
		}
		if (KBot_SignificantlyWeaker(p, self))
		{
			continue;
		}
		if (Visible_360(self, p))
		{
			return false;	// a facing, non-weak, visible enemy: no weave
		}
	}

	return true;
}

// Asymmetric hysteresis wrapper, called once per frame from the moveprobe
// dispatch: condition breaks -> weave drops the SAME frame (vanilla dodge
// takes over immediately); re-engaging requires the condition to hold for
// KBOT_PRED_REARM_SECS continuously.
qbool KBot_PredatorWeave(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	float now = g_globalvars.time;

	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return false;
	}

	if (!KBot_PredatorConditions(self))
	{
		kbot_pred_active[slot] = 0;
		kbot_pred_ok_since[slot] = 0;
		return false;
	}

	if (!kbot_pred_active[slot])
	{
		if ((kbot_pred_ok_since[slot] <= 0) || (now < kbot_pred_ok_since[slot]))
		{
			kbot_pred_ok_since[slot] = now;	// window start (also heals time reset)
		}
		if ((now - kbot_pred_ok_since[slot]) >= KBOT_PRED_REARM_SECS)
		{
			kbot_pred_active[slot] = 1;
		}
	}

	return kbot_pred_active[slot] != 0;
}

// Side-effect-free state query (telemetry mirror in BotLogMoveProbeCommand).
qbool KBot_PredatorWeaveActive(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;

	return (slot >= 0) && (slot < MAX_CLIENTS) && (kbot_pred_active[slot] != 0);
}

#endif // BOT_SUPPORT
