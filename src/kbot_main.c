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

// ---- E3: in-match carve motor (kbot-0.16.0-carve), production, cvar-gated ----
//
// The carve controller (bunnyhop-integration-theory section 5B) rebuilt as a
// stateless-per-frame feedback law. When airborne above a speed floor it sets a
// world-frame wishdir = horizontal velocity rotated by a carve angle chosen by
// intent (c = velocity.wishdir dot: 0 = max-gain straight-line speed build,
// ~29 = soft steering arc toward the nav course, behind-perpendicular = brake
// turn), projected through the CURRENT (aim-owned) view yaw into fmove/smove.
// On the ground-contact frame it re-presses jump (mode-13 anti-pogo latch) to
// chain hops. Stateless per frame apart from a 1-bit side alternator and the
// hop latch (mirrors the E1 rig); no cursor, no activation radius, no map.
//
// CHANNEL CONTRACT (section 5A -- the whole scientific validity of E3): this
// motor writes ONLY the movement channel (direction[0]/direction[1]) and the
// jump bit. It NEVER writes desired_angle/v_angle/view -- combat/aim owns the
// view at ALL times. The view yaw enters BY VALUE (view_yaw), so the motor is
// structurally incapable of steering it; it only projects through it. The hop
// honors combat_nojump (grounded shots win the jump bit).

#define KBOT_CARVE_SPEED_FLOOR 200.0f  // ups below which the motor stays inert
#define KBOT_CARVE_ARC_C       29.0f   // velocity.wishdir dot for the steer arc
#define KBOT_CARVE_BRAKE_DEG   105.0f  // wishdir angle off velocity for a brake turn
#define KBOT_CARVE_DEADZONE    12.0f   // |course error| below this = c=0 straight

static qbool kbot_carve_flip[MAX_CLIENTS];    // side alternator for the c=0 line
static qbool kbot_carve_jlatch[MAX_CLIENTS];  // anti-pogo hop latch (mode-13)
static qbool kbot_carve_hello[MAX_CLIENTS];   // one-time [carve] contract announce

qbool KBot_CarveFrame(gedict_t *self, float view_yaw, qbool combat_nojump,
					  float course_x, float course_y, qbool *jumping,
					  vec3_t direction)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	float speed, rot, side;
	vec3_t cur, course, wish, ang;
	qbool press;

	// INERT PATH: gate OFF, dead, or bad slot -> return false and touch nothing.
	// This is the neutral-off guarantee: at k_kbot_carve 0 the vanilla command
	// (direction, jumping) stands byte-for-byte and the view is never read.
	if ((slot < 0) || (slot >= MAX_CLIENTS) || ISDEAD(self) || !cvar("k_kbot_carve"))
	{
		if ((slot >= 0) && (slot < MAX_CLIENTS))
		{
			kbot_carve_flip[slot] = false;
			kbot_carve_jlatch[slot] = false;
			kbot_carve_hello[slot] = false;
		}
		return false;
	}

	cur[0] = self->s.v.velocity[0];
	cur[1] = self->s.v.velocity[1];
	cur[2] = 0;
	speed = VectorLength(cur);
	if (speed < KBOT_CARVE_SPEED_FLOOR)
	{
		return false; // below the floor -> let vanilla drive, do not perturb
	}

	// One-time channel-contract confirmation at first active frame (match start).
	if (!kbot_carve_hello[slot])
	{
		kbot_carve_hello[slot] = true;
		G_cprint("[carve] motor active slot=%d: view channel NOT owned by carve motor (writes fmove/smove+jump only)\n",
					NUM_FOR_EDICT(self));
	}

	VectorNormalize(cur); // unit horizontal velocity

	// Desired world course (nav dir_move_, horizontal, unit). No course -> hold.
	course[0] = course_x;
	course[1] = course_y;
	course[2] = 0;
	if (VectorNormalize(course) <= 0)
	{
		VectorCopy(cur, course);
	}

	if (onground)
	{
		// Contact frame: run straight along velocity -- the hop below fires this
		// same frame, so ground friction never applies (frictionless hop chain).
		VectorCopy(cur, wish);
	}
	else
	{
		// Signed course error: cross(velocity, course).z picks the side to lean
		// toward; acos(velocity . course) is the unsigned magnitude (both unit).
		float cross_z = cur[0] * course[1] - cur[1] * course[0];
		float dot = DotProduct(cur, course);
		float angerr;
		vec3_t up = { 0, 0, 1 };

		if (dot > 1.0f)  { dot = 1.0f;  }
		if (dot < -1.0f) { dot = -1.0f; }
		angerr = acos(dot) * 180.0f / M_PI;    // 0..180
		side = (cross_z >= 0) ? 1.0f : -1.0f;  // toward the course

		if (angerr < KBOT_CARVE_DEADZONE)
		{
			// Aligned: c = 0 max-gain straight line (wishdir exactly
			// perpendicular), side alternating each frame -> zero net rotation.
			side = kbot_carve_flip[slot] ? 1.0f : -1.0f;
			kbot_carve_flip[slot] = !kbot_carve_flip[slot];
			rot = 90.0f;
		}
		else if (angerr <= 90.0f)
		{
			// Steer arc toward the target: gentle lean, high gain. c ~= 29 ->
			// wishdir at acos(29/speed) off velocity (slight forward pull).
			rot = acos(KBOT_CARVE_ARC_C / speed) * 180.0f / M_PI;
		}
		else
		{
			// Target behind: brake turn -- wishdir slightly behind perpendicular.
			rot = KBOT_CARVE_BRAKE_DEG;
		}

		RotatePointAroundVector(wish, up, cur, rot * side);
		wish[2] = 0;
		VectorNormalize(wish);
	}

	// Anti-pogo hop chain (mode-13 latch): press only on a grounded frame with
	// the button released and no combat veto. Combat wins the jump bit.
	press = onground && !combat_nojump && !kbot_carve_jlatch[slot];
	kbot_carve_jlatch[slot] = press;

	// PROJECTION SEAM: map the world wishdir into fmove/smove through the CURRENT
	// aim-owned view yaw. view_yaw is a read-only copy; desired_angle is NEVER
	// assigned here. trap_makevectors on a LOCAL angle only fills g_globalvars
	// forward/right for the dot projection -- it does not write the view channel.
	VectorSet(ang, 0, view_yaw, 0);
	trap_makevectors(ang);
	direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
	direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
	direction[2] = 0;
	*jumping = press;

	return true; // motor owned movement + jump this frame
}

#endif // BOT_SUPPORT
