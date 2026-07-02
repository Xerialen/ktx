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

// ---- WP4.0 bunny tunables (kbot-0.13.0-bunny) ----
static qbool KBot_BunnyEnabled(void)
{
	return cvar("k_kbot_bunny") != 0;
}

static float KBot_BunnyMaxTurn(void)
{
	float v = cvar("k_kbot_bunny_maxturn");

	return (v <= 0) ? 30.0f : v;
}

static float KBot_BunnyCooldown(void)
{
	float v = cvar("k_kbot_bunny_cooldown");

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
	G_cprint("[kbot-config] ws=%d rockets=%d cells=%d bunny=%d maxturn=%d cooldown=%.1f\n",
				KBot_WeakStack(), KBot_ArmedRockets(), KBot_ArmedCells(),
				KBot_BunnyEnabled() ? 1 : 0, (int)KBot_BunnyMaxTurn(),
				KBot_BunnyCooldown());
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

// ---- WP4.0: bunny travel actuation gate (kbot-0.13.0-bunny) ----
//
// Track B (owner decision): real air-strafe-synced bunny as TRAVEL movement.
// The actuation itself is the intact in-tree mode-23 weave (bot_movement.c:
// per-frame wishdir held acos(numerator/speed) off velocity toward the nav
// bearing -- the cs->0 air-accel law that measured +19% straight-line). This
// gate decides WHEN a kbot may run it; everything about HOW is untouched
// mode-23 machinery (incl. stairs delegation, water fallthrough, and the
// fl_marker-guarded carrot -- both crash guards cherry-picked onto this
// branch). Fleespeed lesson honored: vanilla steering cannot corner in the
// air, so bunny runs ONLY on straight segments and releases to vanilla
// ground movement BEFORE curves. Applies to ALL kbots (tempo is the point);
// travel-only is the gate, never combat.

#define KBOT_BUNNY_PROBE_FWD   80.0f	// forward clearance probe (qu)
#define KBOT_BUNNY_PROBE_UP    56.0f	// headroom probe (player height)
#define KBOT_BUNNY_PROBE_APEX  40.0f	// forward probe at jump-apex height
#define KBOT_BUNNY_MIN_SPEED   100.0f	// below this, heading = route direction

static float kbot_bunny_dmg_given[MAX_CLIENTS];	// last damage DEALT (cooldown)
static int kbot_bunny_active[MAX_CLIENTS];		// engage state (telemetry)

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
	kbot_bunny_dmg_given[slot] = g_globalvars.time;
}

// Cheap conservative corridor check along the flight heading (the fleespeed
// probe pattern): headroom + body-height forward + jump-apex forward.
static qbool KBot_BunnyClearance(gedict_t *self, vec3_t fwd)
{
	vec3_t start, end;

	VectorCopy(self->s.v.origin, start);
	VectorCopy(start, end);
	end[2] += KBOT_BUNNY_PROBE_UP;
	traceline(PASSVEC3(start), PASSVEC3(end), false, self);
	if (g_globalvars.trace_fraction < 1)
	{
		return false;
	}

	VectorMA(self->s.v.origin, KBOT_BUNNY_PROBE_FWD, fwd, end);
	traceline(PASSVEC3(self->s.v.origin), PASSVEC3(end), false, self);
	if (g_globalvars.trace_fraction < 1)
	{
		return false;
	}

	VectorCopy(self->s.v.origin, start);
	start[2] += KBOT_BUNNY_PROBE_APEX;
	VectorMA(start, KBOT_BUNNY_PROBE_FWD, fwd, end);
	traceline(PASSVEC3(start), PASSVEC3(end), false, self);

	return g_globalvars.trace_fraction >= 1;
}

// Per-frame engage decision, called from the moveprobe dispatch. True =
// route this frame through the mode-23 actuation. Every false path drops
// the SAME frame (vanilla movement immediately); the damage/enemy paths
// re-arm only after k_kbot_bunny_cooldown, geometry paths re-arm as soon as
// the segment is straight and clear again.
qbool KBot_BunnyTravel(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	float now = g_globalvars.time;
	float cd = KBot_BunnyCooldown();
	vec3_t cur, bear;
	float delta, speed_sq;

	if ((slot < 0) || (slot >= MAX_CLIENTS))
	{
		return false;
	}
	kbot_bunny_active[slot] = 0;

	if (!KBot_BunnyEnabled())	// k_kbot_bunny 0 = byte-identical vanilla
	{
		return false;
	}
	if (ISDEAD(self) || intermission_running || (self->s.v.waterlevel > 1))
	{
		return false;
	}
	// TRAVEL ONLY: no visible target enemy (frogbots own engagement signal).
	if (self->fb.look_object && (self->fb.look_object->ct == ctPlayer))
	{
		return false;
	}
	// Damage cooldown, given or taken; stale future stamps healed.
	if (self->fb.last_hurt > now)
	{
		self->fb.last_hurt = 0;
	}
	if (kbot_bunny_dmg_given[slot] > now)
	{
		kbot_bunny_dmg_given[slot] = 0;
	}
	if (((now - self->fb.last_hurt) < cd) || ((now - kbot_bunny_dmg_given[slot]) < cd))
	{
		return false;
	}
	// STRAIGHT SEGMENTS ONLY: current heading (horizontal velocity when
	// moving, else the route direction) vs bearing to the linked marker must
	// agree within k_kbot_bunny_maxturn -- release to vanilla BEFORE curves.
	VectorCopy(self->s.v.velocity, cur);
	cur[2] = 0;
	speed_sq = cur[0] * cur[0] + cur[1] * cur[1];
	if (speed_sq < KBOT_BUNNY_MIN_SPEED * KBOT_BUNNY_MIN_SPEED)
	{
		VectorCopy(self->fb.dir_move_, cur);
		cur[2] = 0;
	}
	if (VectorNormalize(cur) <= 0)
	{
		return false;
	}
	if (self->fb.linked_marker && (self->fb.linked_marker != self->fb.touch_marker))
	{
		VectorAdd(self->fb.linked_marker->s.v.absmin,
				  self->fb.linked_marker->s.v.view_ofs, bear);
		VectorSubtract(bear, self->s.v.origin, bear);
	}
	else
	{
		VectorCopy(self->fb.dir_move_, bear);
	}
	bear[2] = 0;
	if (VectorNormalize(bear) <= 0)
	{
		return false;
	}
	delta = vectoyaw(bear) - vectoyaw(cur);
	while (delta > 180.0f) delta -= 360.0f;
	while (delta < -180.0f) delta += 360.0f;
	if ((delta > KBot_BunnyMaxTurn()) || (delta < -KBot_BunnyMaxTurn()))
	{
		return false;
	}
	// CLEARANCE along the flight heading.
	if (!KBot_BunnyClearance(self, cur))
	{
		return false;
	}

	kbot_bunny_active[slot] = 1;
	return true;
}

// Side-effect-free engage-state query (telemetry mirror in the logger).
qbool KBot_BunnyActive(gedict_t *self)
{
	int slot = NUM_FOR_EDICT(self) - 1;

	return (slot >= 0) && (slot < MAX_CLIENTS) && (kbot_bunny_active[slot] != 0);
}

#endif // BOT_SUPPORT
