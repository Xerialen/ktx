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

static float KBot_FleeCooldown(void)
{
	float v = cvar("k_kbot_flee_cooldown");

	return (v <= 0) ? 1.5f : v;
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
	G_cprint("[kbot-config] ws=%d rockets=%d cells=%d flee_cooldown=%.1f\n",
				KBot_WeakStack(), KBot_ArmedRockets(), KBot_ArmedCells(),
				KBot_FleeCooldown());
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
	if ((self->s.v.health + self->s.v.armorvalue) < KBot_WeakStack())
	{
		return true;
	}

	return false;
}

// ---- WP3.7: flee speed (kbot-0.10.0-fleespeed, owner-designed) ----
//
// Speed ONLY on the flee vector: a disciplined-weak kbot (collect/flee mode,
// hunt desire already zeroed by WP3.3) bunnyhops along the route VANILLA
// already chose -- jump is pressed while grounded and moving; steering,
// goals, enemy picking and combat movement are untouched. No enemy-relative
// vectors of any kind (the three enemy-seeking experiments all lost).
//
// Engage requires ALL: kbot; disciplined-weak; no visible target enemy
// (fb.look_object player -- frogbot's own engagement signal); >=
// k_kbot_flee_cooldown seconds since damage GIVEN or TAKEN; corridor
// clearance (cheap conservative traces: headroom + body-height and
// jump-apex-height forward probes along the movement direction -- tight
// passages and ascending stairs stay walked, matching the proven stairs
// doctrine). Drop is IMMEDIATE (same frame) on damage or enemy sight;
// re-arm is cooldown-driven (asymmetric hysteresis, the predator pattern).

#define KBOT_FLEE_PROBE_FWD   80.0f	// forward clearance probe length (qu)
#define KBOT_FLEE_PROBE_UP    56.0f	// headroom probe (player height)
#define KBOT_FLEE_PROBE_APEX  40.0f	// forward probe height at jump apex

static float kbot_flee_dmg_given[MAX_CLIENTS];	// last time this kbot dealt damage
static qbool kbot_flee_jump_press[MAX_CLIENTS];	// jump toggle latch (mode-23 pattern)

// Damage-GIVEN stamp, called from BotDamageInflictedEvent (kbot attackers
// only; damage TAKEN uses the vanilla fb.last_hurt stamp set right there).
void KBot_NoteDamageGiven(gedict_t *attacker)
{
	int slot;

	if (!attacker || !attacker->fb.kbot)
	{
		return;
	}
	slot = NUM_FOR_EDICT(attacker) - 1;
	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return;
	}
	kbot_flee_dmg_given[slot] = g_globalvars.time;
}

// Cheap conservative corridor check: all three traces must be fully open.
static qbool KBot_FleeClearance(gedict_t *self, vec3_t fwd)
{
	vec3_t start, end;

	// Headroom for the jump itself.
	VectorCopy(self->s.v.origin, start);
	VectorCopy(start, end);
	end[2] += KBOT_FLEE_PROBE_UP;
	traceline(PASSVEC3(start), PASSVEC3(end), false, self);
	if (g_globalvars.trace_fraction < 1)
	{
		return false;
	}

	// Forward at body height.
	VectorMA(self->s.v.origin, KBOT_FLEE_PROBE_FWD, fwd, end);
	traceline(PASSVEC3(self->s.v.origin), PASSVEC3(end), false, self);
	if (g_globalvars.trace_fraction < 1)
	{
		return false;
	}

	// Forward at jump-apex height (catches ascending stairs / low ceilings).
	VectorCopy(self->s.v.origin, start);
	start[2] += KBOT_FLEE_PROBE_APEX;
	VectorMA(start, KBOT_FLEE_PROBE_FWD, fwd, end);
	traceline(PASSVEC3(start), PASSVEC3(end), false, self);

	return g_globalvars.trace_fraction >= 1;
}

// Per-frame flee-speed jump decision, called from BotSetCommand BEFORE all
// vanilla overrides (dead/prewar/lab-moveprobe all retain authority). Only
// ever ADDS a jump press to otherwise-unchanged vanilla movement.
qbool KBot_FleeSpeedJump(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	float now = g_globalvars.time;
	float cd = KBot_FleeCooldown();
	vec3_t fwd;
	qbool press;

	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return false;
	}
	if (ISDEAD(self) || intermission_running || (self->s.v.waterlevel > 1))
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}
	// Flee/collect mode only: the same predicate that zeroes hunt desire.
	if (!KBot_AvoidFights(self))
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}
	// INSTANT DROP: visible target enemy...
	if (self->fb.look_object && (self->fb.look_object->ct == ctPlayer))
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}
	// ...or recent damage taken/given (cooldown-driven re-arm). Stale future
	// stamps (map/time reset) are healed to "long ago".
	if (self->fb.last_hurt > now)
	{
		self->fb.last_hurt = 0;
	}
	if (kbot_flee_dmg_given[slot] > now)
	{
		kbot_flee_dmg_given[slot] = 0;
	}
	if (((now - self->fb.last_hurt) < cd) || ((now - kbot_flee_dmg_given[slot]) < cd))
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}
	// Moving along the vanilla-chosen route (never pogo in place).
	VectorCopy(self->fb.dir_move_, fwd);
	fwd[2] = 0;
	if (VectorNormalize(fwd) <= 0)
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}
	if (!((int)self->s.v.flags & FL_ONGROUND))
	{
		kbot_flee_jump_press[slot] = false;	// press on the next landing frame
		return false;
	}
	if (!KBot_FleeClearance(self, fwd))
	{
		kbot_flee_jump_press[slot] = false;
		return false;
	}

	// Toggle latch (mode-23 pattern): alternate press/release on grounded
	// frames so the engine sees a fresh +jump each hop.
	press = !kbot_flee_jump_press[slot];
	kbot_flee_jump_press[slot] = press;

	return press;
}

#endif // BOT_SUPPORT
