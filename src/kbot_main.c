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
	if ((self->s.v.health + self->s.v.armorvalue) < KBot_WeakStack())
	{
		return true;
	}

	return false;
}


// ---- E1: carve-law verification rig (kbot-0.15.0-e1, lab mode) ----
//
// Verifies the bunnyhop-integration-theory decoupling theorem live (theory
// doc section 2.2 + experiment E1): wishdir depends only on view yaw and the
// fmove:smove ratio, so a bot can air-strafe optimally with the VIEW PINNED.
// Two arms, k_kbot_e1_mode: 1 = c=0 carve (world wishdir = horizontal
// velocity rotated exactly 90 deg, side ALTERNATING EVERY FRAME -> zero net
// rotation, straight line); 2 = gen-1 mode-13 law (wishdir at
// acos(26/speed) off velocity, single-sided) as the control arm. Both arms:
// view yaw pinned to k_kbot_e1_yaw (written as the same constant every
// frame -- never steered), wishdir projected through the pinned yaw into
// fmove/smove (the bot_movement.c dir_move_ projection seam), jump released
// and re-pressed on the exact ground-contact frame (mode-13 toggle latch =
// frame-perfect, friction-free hops). Telemetry: grep-able [e1] lines every
// ~0.1 s with horizontal speed and the ACTUAL server-side view yaw (proves
// the pin). Passes auto-reset by teleport to k_kbot_e1_start every
// k_kbot_e1_pass seconds. Lab-only: inert at k_kbot_e1_mode 0 (default).

static int e1_mode_active[MAX_CLIENTS];
static int e1_pass_num[MAX_CLIENTS];
static float e1_pass_start[MAX_CLIENTS];
static float e1_flip[MAX_CLIENTS];
static qbool e1_jump_latch[MAX_CLIENTS];
static float e1_last_log[MAX_CLIENTS];

static void KBot_E1Teleport(gedict_t *self)
{
	char buf[64];
	float x, y, z;
	float v0 = cvar("k_kbot_e1_v0");
	float pinned_yaw = cvar("k_kbot_e1_yaw");

	trap_cvar_string("k_kbot_e1_start", buf, sizeof(buf));
	if (buf[0] && (sscanf(buf, "%f %f %f", &x, &y, &z) == 3))
	{
		vec3_t org;

		VectorSet(org, x, y, z);
		setorigin(self, PASSVEC3(org));
	}

	// Initial condition: inject the bot already moving at run speed along the
	// pinned heading (k_kbot_e1_v0, default 320 ups). The carve LAW is a
	// steady-state gain (d|v|^2/dt = 900 - c^2); the pre-registered acceptance
	// targets (490@2s, 610@4s) assume the theory premise v0=320 at t=0. A
	// teleport-to-rest starts the clock ~0.4 s early on a cold-speed bootstrap
	// (0 -> 320) that is NOT part of the law under test, time-shifting every
	// sample and under-reading @2s. Seeding v0 removes that rig artifact so the
	// measured curve is the carve law itself. v0 <= 0 keeps the old rest start.
	if (v0 > 0.0f)
	{
		vec3_t ang = { 0, 0, 0 }, fwd;

		ang[YAW] = pinned_yaw;
		trap_makevectors(ang);
		VectorCopy(g_globalvars.v_forward, fwd);
		fwd[2] = 0;
		VectorNormalize(fwd);
		VectorScale(fwd, v0, self->s.v.velocity);
	}
	else
	{
		VectorSet(self->s.v.velocity, 0, 0, 0);
	}
}

// Returns true when the lab mode owns this frame (BotSetCommand then sends
// exactly our command). False = inert, vanilla untouched.
qbool KBot_E1Frame(gedict_t *self, qbool *jumping, qbool *firing, int *impulse,
				   vec3_t direction)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	int mode = (int)cvar("k_kbot_e1_mode");
	float now = g_globalvars.time;
	float pass_len = cvar("k_kbot_e1_pass");
	float pinned_yaw = cvar("k_kbot_e1_yaw");
	float speed, rot;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	qbool press;
	vec3_t cur, wish, ang;

	if ((mode < 1) || (mode > 2) || (slot < 0) || (slot >= MAX_CLIENTS) || ISDEAD(self))
	{
		if ((slot >= 0) && (slot < MAX_CLIENTS))
		{
			e1_mode_active[slot] = 0;
		}
		return false;
	}
	if (pass_len < 2)
	{
		pass_len = 6.0f;
	}

	// (Re)init on mode change; pass reset on timer (also heals time resets).
	if (e1_mode_active[slot] != mode)
	{
		e1_mode_active[slot] = mode;
		e1_pass_num[slot] = 1;
		e1_pass_start[slot] = now;
		e1_flip[slot] = 1;
		e1_jump_latch[slot] = false;
		e1_last_log[slot] = 0;
		KBot_E1Teleport(self);
		G_cprint("[e1] init mode=%d pass=1 yaw_pin=%.2f\n", mode, pinned_yaw);
	}
	else if (((now - e1_pass_start[slot]) >= pass_len) || (e1_pass_start[slot] > now))
	{
		e1_pass_num[slot]++;
		e1_pass_start[slot] = now;
		e1_flip[slot] = 1;
		e1_jump_latch[slot] = false;
		KBot_E1Teleport(self);
		G_cprint("[e1] pass=%d mode=%d\n", e1_pass_num[slot], mode);
	}

	cur[0] = self->s.v.velocity[0];
	cur[1] = self->s.v.velocity[1];
	cur[2] = 0;
	speed = VectorLength(cur);

	if ((speed < 200) || (VectorNormalize(cur) <= 0))
	{
		// Bootstrap to run speed straight down the runway (pinned heading).
		vec3_t fwd_ang = { 0, 0, 0 };

		fwd_ang[YAW] = pinned_yaw;
		trap_makevectors(fwd_ang);
		VectorCopy(g_globalvars.v_forward, wish);
		wish[2] = 0;
		VectorNormalize(wish);
	}
	else if (onground)
	{
		// Contact frame: run straight along velocity (mode-13 pattern); the
		// jump below fires this exact frame, so friction never applies.
		VectorCopy(cur, wish);
	}
	else if (mode == 1)
	{
		// ARM A: c = 0 carve. Exactly perpendicular, side alternating EVERY
		// frame -> zero net rotation, straight line, max gain (900 - c^2).
		vec3_t up = { 0, 0, 1 };

		RotatePointAroundVector(wish, up, cur, 90.0f * e1_flip[slot]);
		e1_flip[slot] = -e1_flip[slot];
		wish[2] = 0;
		VectorNormalize(wish);
	}
	else
	{
		// ARM B: gen-1 mode-13 law -- hold c ~= 26 (wishdir acos(26/speed)
		// off velocity), single-sided (its default). Same rig otherwise.
		vec3_t up = { 0, 0, 1 };
		float k = 26.0f;
		float ratio = (speed > k) ? (k / speed) : 1.0f;

		rot = acos(ratio) * 180.0f / M_PI;
		RotatePointAroundVector(wish, up, cur, rot);
		wish[2] = 0;
		VectorNormalize(wish);
	}

	// Frame-perfect anti-pogo hop chain (mode-13 toggle latch): press only on
	// a grounded frame with the button released, release otherwise.
	press = onground && (speed >= 200) && !e1_jump_latch[slot];
	e1_jump_latch[slot] = press;

	// VIEW PIN + projection seam: the same world->local projection the
	// vanilla emitter uses for dir_move_, through the PINNED yaw. The view
	// never steers; fmove/smove carry the whole carve.
	VectorSet(ang, 0, pinned_yaw, 0);
	trap_makevectors(ang);
	self->fb.desired_angle[PITCH] = 0;
	self->fb.desired_angle[YAW] = pinned_yaw;
	self->fb.desired_angle[ROLL] = 0;
	direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
	direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
	direction[2] = 0;
	*jumping = press;
	*firing = false;
	*impulse = 0;

	// Telemetry ~10 Hz: t, arm, pass, horizontal speed, the PINNED yaw and
	// the ACTUAL server-side view yaw (v_angle -- proves the pin held),
	// ground flag, position (straight-line verification).
	if (((now - e1_last_log[slot]) >= 0.1f) || (e1_last_log[slot] > now))
	{
		e1_last_log[slot] = now;
		G_cprint("[e1] t=%.3f mode=%d pass=%d speed=%.1f yaw=%.2f vyaw=%.2f og=%d pos=%.0f,%.0f,%.0f\n",
					now, mode, e1_pass_num[slot], speed, pinned_yaw,
					self->s.v.v_angle[YAW], onground ? 1 : 0,
					self->s.v.origin[0], self->s.v.origin[1], self->s.v.origin[2]);
	}

	return true;
}

#endif // BOT_SUPPORT
