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

// ---- WP3.2: team blackboard ----
//
// Team-level coordination inputs the vanilla economy then acts on. Two
// mechanisms, both kbot-gated on the write AND read side:
// 1. FOCUS FIRE: BestEnemy_apply subtracts KBot_FocusFireBonus seconds from
//    the (travel-time-shaped) enemy score for enemies a teammate is already
//    engaging and for enemies already hurt -- 2-on-1s convert, 4-way damage
//    splits do not.
// 2. ITEM-CLAIM DEDUP: when a kbot commits to a major item (RA/YA/mega/quad)
//    in UpdateGoal, it claims it on a per-slot blackboard (TTL'd, refreshed
//    by the ~2 s goal cadence, released on re-commit elsewhere); EvalGoal
//    suppresses the same goal for teammates with a live CLOSER claim, so the
//    vanilla economy picks their next-best goal instead.
//
// Provenance: the hurt-enemy check reads exact enemy health (server-side-
// omniscient, like parts of the vanilla economy); the teammate look_object
// and claim reads are team-internal knowledge. Lab-bench acceptable per PRD;
// flagged for any future public-server readiness review.

#define KBOT_CLAIM_TTL          5.0f	// claim lifetime; goal refresh (~2 s) renews it
#define KBOT_FF_TEAMMATE_BONUS  3.0f	// score seconds off enemies a teammate engages
#define KBOT_FF_HURT_HEALTH     60.0f	// enemy at or below this health counts as hurt
#define KBOT_FF_HURT_BONUS      1.5f	// score seconds off hurt enemies

typedef struct kbot_claim_s
{
	gedict_t *item;		// claimed major item (NULL = no claim)
	float expires;		// absolute server time
} kbot_claim_t;

static kbot_claim_t kbot_claims[MAX_CLIENTS];	// indexed by owner slot

// Majors eligible for claims: RA / YA / mega / quad (per WP3.2 spec).
static qbool KBot_IsMajorItem(gedict_t *item)
{
	const char *cn;

	if (!item || !item->classname)
	{
		return false;
	}
	cn = item->classname;
	if (streq(cn, "item_armorInv") || streq(cn, "item_armor2")
		|| streq(cn, "item_artifact_super_damage"))
	{
		return true;
	}

	return streq(cn, "item_health") && ((int)item->s.v.spawnflags & H_MEGA);
}

// Publish this kbot's committed goal. A non-major (or NULL) commitment
// releases any previous claim. Called from UpdateGoal's tail, so the ~2 s
// goal cadence refreshes live claims; death is handled on the read side.
void KBot_ClaimGoal(gedict_t *self, gedict_t *goal)
{
	int slot = NUM_FOR_EDICT(self) - 1;

	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return;
	}
	if (goal && KBot_IsMajorItem(goal))
	{
		kbot_claims[slot].item = goal;
		kbot_claims[slot].expires = g_globalvars.time + KBOT_CLAIM_TTL;
	}
	else
	{
		kbot_claims[slot].item = NULL;
		kbot_claims[slot].expires = 0;
	}
}

// True when a live same-team kbot holds a fresh claim on this major item AND
// is strictly closer -- the caller (EvalGoal, kbot-gated) then skips the
// goal so the vanilla economy picks something else. Never suppresses
// non-majors, never suppresses the claim owner itself.
qbool KBot_GoalClaimedByTeammate(gedict_t *self, gedict_t *goal)
{
	gedict_t *p;
	vec3_t d;
	float my_d, now = g_globalvars.time;

	if (!teamplay || !goal || !KBot_IsMajorItem(goal))
	{
		return false;
	}

	VectorSubtract(goal->s.v.origin, self->s.v.origin, d);
	my_d = VectorLength(d);

	for (p = world; (p = find_plr(p));)
	{
		int slot;

		if ((p == self) || !p->fb.kbot || ISDEAD(p) || !SameTeam(p, self))
		{
			continue;
		}
		slot = NUM_FOR_EDICT(p) - 1;
		if ((slot < 0) || (slot >= MAX_CLIENTS))
		{
			continue;
		}
		if (kbot_claims[slot].item != goal)
		{
			continue;
		}
		if (kbot_claims[slot].expires > now + KBOT_CLAIM_TTL)
		{
			kbot_claims[slot].expires = 0;	// stale future stamp (map/time reset)
			continue;
		}
		if (now >= kbot_claims[slot].expires)
		{
			continue;	// expired
		}
		VectorSubtract(goal->s.v.origin, p->s.v.origin, d);
		if (VectorLength(d) < my_d)
		{
			return true;
		}
	}

	return false;
}

// Focus-fire bonus in SCORE SECONDS (BestEnemy_apply's enemy_score is a
// travel time): subtracted for enemies a live teammate is already engaging
// (their look_object) and for enemies already hurt. The teammate bonus does
// not stack -- one 2-on-1 conversion, not a 4-stack on one target.
float KBot_FocusFireBonus(gedict_t *self, gedict_t *enemy)
{
	gedict_t *p;
	float bonus = 0;

	if (!enemy || ISDEAD(enemy))
	{
		return 0;
	}
	if (enemy->s.v.health <= KBOT_FF_HURT_HEALTH)
	{
		bonus += KBOT_FF_HURT_BONUS;
	}
	for (p = world; (p = find_plr(p));)
	{
		if ((p == self) || ISDEAD(p) || !SameTeam(p, self))
		{
			continue;
		}
		if (p->fb.look_object == enemy)
		{
			bonus += KBOT_FF_TEAMMATE_BONUS;
			break;
		}
	}

	return bonus;
}

#endif // BOT_SUPPORT
