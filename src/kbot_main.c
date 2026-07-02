/*
 kbot_main.c -- KomodoBrain skeleton (WP2.1) + discipline (WP3.3) + tunables (WP3.5)

 Proves the brain seam: KBot_MarkBot() flags a bot as a komodobot and stamps
 its identity into run evidence; KBot_Frame() is the per-frame entry point,
 which delegates 100% to the stock frogbot logic.
 */
#ifdef BOT_SUPPORT

#include "g_local.h"
#include "kbot.h"

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

	// Identity markers: userinfo key + "kb:" name prefix, so identity shows
	// up in ktxstats / MVD player names.
	trap_SetBotUserInfo(entity, "kbot", stamped, 0);
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
		localcmd("serverinfo kbot_version %s\n", stamped);
	}

	G_cprint("[kbot] slot=%d name=%s version=%s\n", entity, bot->netname, stamped);
	// Ledger honesty (WP3.5): record the effective tunables with every stamp
	// so run evidence captures the swept settings without perturbing the
	// identity match above.
	KBot_LogConfig();
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
// "Armed" = a real duel weapon with ammo to use it (tunable thresholds).
static qbool KBot_Armed(gedict_t *p)
{
	int held = (int)p->s.v.items;

	return ((held & IT_ROCKET_LAUNCHER) && (p->s.v.ammo_rockets > KBot_ArmedRockets()))
			|| ((held & IT_LIGHTNING) && (p->s.v.ammo_cells > KBot_ArmedCells()));
}

qbool KBot_AvoidFights(gedict_t *self)
{
	if (!KBot_Armed(self))
	{
		return true;
	}
	if ((self->s.v.health + self->s.v.armorvalue) < KBot_WeakStack())
	{
		return true;
	}

	return false;
}

// ---- WP3.6: press (kbot-0.9.0) -- the mirror of discipline ----
//
// Motivating data: kbots out-damage frogs by ~15k/batch at only +7..9
// margin -- wounded enemies escape, re-stack, return. When a kbot is STRONG
// (not KBot_AvoidFights) and a visible enemy is WEAK (stack trails ours by
// k_kbot_press_margin, or unarmed-vs-armed), press the kill at the GOAL
// level only: boost hunt desire in UpdateGoal and (bounded) bias enemy
// picking. Zero movement changes -- predator taught us the movement-level
// version fails.
//
// Provenance: stack/ammo reads are server-side-omniscient (like the vanilla
// economy's own desire inputs). Lab-bench acceptable per PRD; flagged for
// any future public-server readiness review.

static float KBot_PressMargin(void)
{
	float v = cvar("k_kbot_press_margin");

	return (v <= 0) ? 50.0f : v;
}

static float KBot_PressMemory(void)
{
	float v = cvar("k_kbot_press_memory");

	return (v <= 0) ? 2.0f : v;
}

// Log the effective tunables (called from the [kbot] stamp).
void KBot_LogConfig(void)
{
	G_cprint("[kbot-config] ws=%d rockets=%d cells=%d press_margin=%d press_memory=%.1f\n",
				KBot_WeakStack(), KBot_ArmedRockets(), KBot_ArmedCells(),
				(int)KBot_PressMargin(), KBot_PressMemory());
}

// p is significantly weaker than self (same shape the predator experiment
// used): lacks a usable duel weapon while we are armed, or their stack
// trails ours by more than the press margin.
static qbool KBot_SignificantlyWeaker(gedict_t *p, gedict_t *self)
{
	if (!KBot_Armed(p) && KBot_Armed(self))
	{
		return true;
	}

	return (p->s.v.health + p->s.v.armorvalue)
			< (self->s.v.health + self->s.v.armorvalue - KBot_PressMargin());
}

// Visible-or-recently-seen memory, per slot: pressing persists for
// k_kbot_press_memory seconds after the target was last visible, and only
// for the SAME target (a different enemy must be seen to start a new press).
static gedict_t *kbot_press_target[MAX_CLIENTS];
static float kbot_press_seen[MAX_CLIENTS];

// Press predicate for the UpdateGoal hunt hook: strong self, weak visible
// (or recently seen) enemy. Updates the last-seen memory as a side effect;
// call it once per goal refresh (it is).
qbool KBot_PressEnemy(gedict_t *self, gedict_t *enemy)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	float now = g_globalvars.time;

	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return false;
	}
	if (!enemy || (enemy->ct != ctPlayer) || ISDEAD(enemy) || SameTeam(self, enemy))
	{
		return false;
	}
	if (ISDEAD(self) || KBot_AvoidFights(self))
	{
		return false;	// stack dropped mid-chase: discipline takes back over
	}
	if (!KBot_SignificantlyWeaker(enemy, self))
	{
		return false;
	}

	if (Visible_360(self, enemy))
	{
		kbot_press_target[slot] = enemy;
		kbot_press_seen[slot] = now;
		return true;
	}

	// Recently-seen memory (same target only); heal stale/future stamps.
	if (kbot_press_target[slot] != enemy)
	{
		return false;
	}
	if ((kbot_press_seen[slot] > now) || ((now - kbot_press_seen[slot]) > KBot_PressMemory()))
	{
		kbot_press_target[slot] = NULL;
		kbot_press_seen[slot] = 0;
		return false;
	}

	return true;
}

// Bounded enemy-pick bias for BestEnemy_apply, in SCORE SECONDS (the score
// is a travel time; lower wins). Flat and capped at 1.5 s, which IS the
// locality bound: a weak enemy can only win the pick if it is within 1.5 s
// of the best candidate -- never a cross-map chase (the focus-fire lesson).
// Side-effect free: current visibility only, no memory update.
float KBot_PressPickBias(gedict_t *self, gedict_t *enemy)
{
	if (!enemy || (enemy->ct != ctPlayer) || ISDEAD(enemy) || SameTeam(self, enemy))
	{
		return 0;
	}
	if (ISDEAD(self) || KBot_AvoidFights(self))
	{
		return 0;
	}
	if (!KBot_SignificantlyWeaker(enemy, self))
	{
		return 0;
	}
	if (!Visible_360(self, enemy))
	{
		return 0;
	}

	return 1.5f;
}

#endif // BOT_SUPPORT
