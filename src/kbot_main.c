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

#endif // BOT_SUPPORT
