/*
 kbot_main.c -- KomodoBrain skeleton (WP2.1) + discipline (WP3.3) + tunables (WP3.5)

 Proves the brain seam: KBot_MarkBot() flags a bot as a komodobot and stamps
 its identity into run evidence; KBot_Frame() is the per-frame entry point,
 which delegates 100% to the stock frogbot logic.
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"

// Cached cvar read shared across the kbot seams (declared in kbot.h): reads
// `name` at most ~once per second into *val, gating on *next against the server
// clock, with a rewind guard for map change / restart. Behaviour-neutral
// promotion of the former per-file KHW_Cvar / KHV_CvarCached twins.
float KBot_CvarCached(const char *name, float *val, float *next)
{
	if (*next > g_globalvars.time + 3)
	{
		*next = -1; // clock rewound (map change/restart)
	}
	if (g_globalvars.time > *next)
	{
		*val = cvar(name);
		*next = g_globalvars.time + 1;
	}

	return *val;
}

// Effective identity stamp: KBOT_VERSION + k_kbot_version_suffix (set by the
// bench cfg together with --candidate-version, so the observed-identity gate
// still matches stamp == roster.candidate_version when sweeping tunables).
static void KBot_StampedVersion(char *out, int out_size)
{
	char suffix[32];

	trap_cvar_string("k_kbot_version_suffix", suffix, sizeof(suffix));
	snprintf(out, out_size, "%s%s", KBOT_VERSION, suffix);
}

// Discipline tunables (WP3.5): cvar-backed so the bench can sweep them from
// the server cfg without rebuilds ("set k_kbot_weak_stack 100"). Registered
// with real defaults in world.c; the <= 0 fallbacks additionally reproduce
// the 0.5.0 constants if registration is ever skipped. Explicit 0 is
// therefore not expressible -- sweeps use positive values only.
static int KBot_WeakStack(void)
{
	int v = (int)cvar("k_kbot_weak_stack");

	return (v <= 0) ? 70 : v;
}

static int KBot_ArmedRockets(void)
{
	int v = (int)cvar("k_kbot_weak_rockets");

	return (v <= 0) ? 3 : v;
}

static int KBot_ArmedCells(void)
{
	int v = (int)cvar("k_kbot_weak_cells");

	return (v <= 0) ? 15 : v;
}

void KBot_MarkBot(gedict_t *bot)
{
	char newname[CLIENT_NAME_LEN];
	char infobuf[64];
	char stamped[64];
	int entity;

	if (!bot || !bot->isBot)
	{
		return;
	}

	entity = NUM_FOR_EDICT(bot);
	bot->fb.kbot = KBOT_STATE_MARKED;
	KBot_StampedVersion(stamped, sizeof(stamped));

	// Identity marker: the "kbot" userinfo key + the [kbot] stamp line below
	// are the identity proof (ledger maps stamps onto roster names).
	trap_SetBotUserInfo(entity, "kbot", stamped, 0);

	// Owner roster rule (2026-07-06): komodobots field the owner's chosen
	// names (k_kbot_name1..4, default hib/dag/Angua/Rock) and color
	// (k_kbot_color, default 3). Same cvar interface as the mm2humanmode
	// branch so the branches merge cleanly. Team seating stays with the
	// bench (k_kbot_team registered for interface parity only). Index = how
	// many kbots are already marked, in join order. Empty name cvar falls
	// back to the legacy kb: prefix.
	{
		char namecvar[16];
		char color[8];
		gedict_t *p;
		int idx = 1;

		for (p = world; (p = find_plr(p));)
		{
			if ((p != bot) && p->isBot && p->fb.kbot)
			{
				idx++;
			}
		}
		bot->fb.kbot_slot = idx;	// stable per-bot identity for dial overrides
		snprintf(namecvar, sizeof(namecvar), "k_kbot_name%d", idx);
		trap_cvar_string(namecvar, newname, sizeof(newname));
		if (!strnull(newname))
		{
			trap_SetBotUserInfo(entity, "name", newname, 0);
			infokey(bot, "name", bot->netname, CLIENT_NAME_LEN);
		}
		else if (strncmp(bot->netname, "kb:", 3))
		{
			snprintf(newname, sizeof(newname), "kb:%s", bot->netname);
			trap_SetBotUserInfo(entity, "name", newname, 0);
			infokey(bot, "name", bot->netname, CLIENT_NAME_LEN);
		}
		trap_cvar_string("k_kbot_color", color, sizeof(color));
		if (!strnull(color))
		{
			trap_SetBotUserInfo(entity, "topcolor", color, 0);
			trap_SetBotUserInfo(entity, "bottomcolor", color, 0);
		}
	}

	// Advertise the brain version via serverinfo (set once, on first kbot).
	infokey(world, "kbot_version", infobuf, sizeof(infobuf));
	if (strnull(infobuf))
	{
		localcmd("serverinfo kbot_version %s\n", stamped);
	}

	G_cprint("[kbot] slot=%d name=%s version=%s\n", entity, bot->netname, stamped);
	// Ledger honesty (WP3.5): record the effective tunables with every stamp
	// so run evidence captures the swept settings without perturbing the
	// identity match above.
	G_cprint("[kbot-config] ws=%d rockets=%d cells=%d\n",
				KBot_WeakStack(), KBot_ArmedRockets(), KBot_ArmedCells());
}

// Per-frame brain entry point. WP2.1: pure delegation -- log identity once,
// then return false so BotsThinkTime() runs the stock frogbot logic unchanged.
qbool KBot_Frame(gedict_t *self)
{
	if (self->fb.kbot == KBOT_STATE_MARKED)
	{
		char stamped[64];

		KBot_StampedVersion(stamped, sizeof(stamped));
		self->fb.kbot = KBOT_STATE_ACTIVE;
		G_cprint("[kbot] frame active slot=%d name=%s version=%s time=%f\n",
					NUM_FOR_EDICT(self), self->netname, stamped,
					g_globalvars.time);
	}

	return false; // not handled: fall through to stock frogbot think
}

// ---- WP3.3: engage/disengage discipline ----

// True when this kbot should decline to HUNT (goal-level only): it has no
// usable duel weapon ("armed" = RL with rockets / LG with cells, thresholds
// per the codebase's own AttackRespawns convention), or its stack is
// critically low. Fresh spawns on weapon-stripped maps are disarmed by
// definition -- this is the post-death discipline: collect armor/weapon
// first, re-engage once armed. Deliberately side-effect free and marker-free
// (reads only self->s.v scalars). Called at goal-refresh cadence (~0.5 Hz per
// bot), so the cvar() trap reads are not hot-path.
qbool KBot_AvoidFights(gedict_t *self)
{
	int held = (int)self->s.v.items;
	qbool armed = ((held & IT_ROCKET_LAUNCHER) && (self->s.v.ammo_rockets > KBot_ArmedRockets()))
			|| ((held & IT_LIGHTNING) && (self->s.v.ammo_cells > KBot_ArmedCells()));

	if (!armed)
	{
		return true;
	}
	// D1 engage dial: low aggression retreats earlier, high fights lower
	if ((self->s.v.health + self->s.v.armorvalue)
			< KBot_WeakStack() * KBot_DialWeakScale(self))
	{
		return true;
	}

	return false;
}

// ---- decision model v1: route focus (owner doctrine 2026-07-05) ----
//
// A weak kbot (KBot_AvoidFights) routing to a world item ignores enemies
// entirely: no retarget, no chase, no combat-driven goal refresh. The
// decision-log baseline showed 91% of weak-bot damage events fire while the
// goal is a world item -- combat micro blind to the route is how the bot
// "jumps down shooting from quad" and loses its position. Two doctrine
// exceptions re-enable engagement:
//   (a) finish-off: the enemy carries a real weapon (RL/LG) AND is low
//       health (<= k_kbot_finish_hp) -- a couple of shotgun shells convert.
//   (b) jump-denial (enemy mid gap-jump) -- NOT implemented in v1; needs the
//       gj-lane flight zones, documented in the model spec.
// Gated per-call by k_kbot_route_focus; baseline bots can't take the branch.
qbool KBot_RouteFocusIgnore(gedict_t *self, gedict_t *enemy)
{
	gedict_t *goal;

	if (!self->isBot || !self->fb.kbot || !cvar("k_kbot_route_focus"))
	{
		return false;
	}
	// SG-only, literally (owner doctrine): any real weapon means the bot
	// fights as vanilla. The stack-based weak test (KBot_AvoidFights) proved
	// far too broad here -- an RL-carrier at stack 90 refusing to fight gets
	// farmed (wave-1 A/B 2026-07-05: mean -54.75 over 4 matches, 0 wins).
	if ((int)self->s.v.items & (IT_ROCKET_LAUNCHER | IT_LIGHTNING | IT_GRENADE_LAUNCHER
			| IT_SUPER_NAILGUN | IT_NAILGUN | IT_SUPER_SHOTGUN))
	{
		return false;
	}
	goal = &g_edicts[(int)self->s.v.goalentity];
	if ((goal == world) || (goal->ct == ctPlayer))
	{
		return false; // no route to protect
	}
	if (enemy && (enemy->ct == ctPlayer)
			&& ((int)enemy->s.v.items & (IT_ROCKET_LAUNCHER | IT_LIGHTNING))
			&& (enemy->s.v.health <= max(1, (int)cvar("k_kbot_finish_hp"))))
	{
		return false; // exception (a): finish-off
	}

	return true;
}


#endif // BOT_SUPPORT
