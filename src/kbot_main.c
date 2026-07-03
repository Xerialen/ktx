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

// ============================================================================
//  E6: gap-crossing strafe-jump play (kbot-0.17.0-gapjump)
// ============================================================================
//
// ONE technique: a horizontal strafe/speed-jump across dm3's central gap. Run
// on the takeoff ledge with horizontal speed, hop at the edge, air-strafe
// across, land on the far ledge. NO rocket-jump. Two parallel lanes, both
// directions: Ring<->Quad and RA-entrance<->YA-high.
//
// Air-control engine = the E1 c=0 alternating carve (bunnyhop decoupling
// theorem): airborne wishdir is the horizontal velocity rotated +/-90 deg.
// Alternating the sign every frame -> zero net rotation -> straight line at
// max speed gain (900 - c^2). To STEER toward the landing we bias the sign:
// while the velocity bearing is off the bearing-to-landing by more than a
// deadband we hold the perpendicular that rotates velocity toward the target
// (bang-bang air-strafe turn); inside the deadband we alternate (go straight,
// keep speed). The frame-perfect contact-frame hop latch (press jump only on
// a grounded frame with the button released) gives friction-free hops.
//
// The wishdir is projected into fmove/smove through the view yaw at the
// bot_movement.c dir_move_ seam exactly like E1 (the view yaw cancels out of
// wishvel, so aim stays free for the combat layer in passive mode).
//
// Two ways to run:
//   * TRIAL DRIVER (E6 measurement): k_kbot_gj_lane in [0..3] selects a lane;
//     the code teleports the bot to the takeoff, seeds run speed, executes the
//     crossing, detects LAND vs FAIL, logs grep-able [gapjump] telemetry, and
//     auto-repeats. This is the isolated jump-landing harness (>=50 trials).
//   * PASSIVE TRIGGER (the real feature): k_kbot_gapjump 1, k_kbot_gj_lane -1.
//     When the bot is in a takeoff zone, its nav goal is across the gap, and
//     NO enemy is near (combat-yield), it executes ONE crossing, then releases
//     movement back to vanilla nav on land.
//
// Neutral-off: k_kbot_gapjump 0 => this function returns false on the very
// first check, so the emitted command is byte-for-byte vanilla.

char* LocationName(float x, float y, float z); // teamplay.c

// Per-lane geometry, discovered in SERVER (setorigin) coordinates by observing
// where a live frogbot stands at each item (k_kbot_gj_probe) + dm3.loc/8. The
// takeoff origin sits on the ledge a short run back from the lip; the landing
// origin is the centre of the far ledge platform. fail_z is the height below
// which the bot has fallen into the central void.
typedef struct
{
	const char *name;
	vec3_t takeoff;
	vec3_t landing;
	float fail_z;
} gj_lane_t;

#define GJ_NUM_LANES 4
static const gj_lane_t gj_lanes[GJ_NUM_LANES] = {
	// 0: ring -> quad. +X level leap over the central hill-pit (traced floor
	// ~-224, 256 deep below the z=32 ledges; player-rest z=56). The central
	// PILLAR walls off y>=160 (near-wall at x~512); the OPEN corridor is
	// y=-160..140, verified wall-free from x430 to x1024 at all heights. Cross
	// at y=40 (open, drift margin). Takeoff is the LEDGE EDGE (ring x~360, quad
	// x~796) so the single strafe-hop arc apexes over the void (not over solid
	// ground); run runup 0 so the launch hop fires at the edge. ~436u open gap.
	// fail_z=-40 (below the ledge = fell into pit).
	{ "ring2quad", { 360, 40, 56 }, { 796, 40, 56 }, -40 },
	// 1: quad -> ring
	{ "quad2ring", { 796, 40, 56 }, { 360, 40, 56 }, -40 },
	// 2: ra-entrance -> ya-high. The literal RA-entry (x480,z56) and YA ledges
	// (x600+,z88) are separated by a WALL (x520-560, solid >=z140), NOT a clean
	// +X void; the only parallel-to-ring<->quad cross-void is the central pit's
	// SOUTHERN flank (open corridor y=-160..-80). So this lane crosses that pit
	// at y=-130 (west rim toward RA <-> east rim toward YA) -- same technique,
	// distinct lane, +X level ~436u. fail_z=-40.
	{ "ra2ya", { 360, -130, 56 }, { 796, -130, 56 }, -40 },
	// 3: ya-high -> ra-entrance
	{ "ya2ra", { 796, -130, 56 }, { 360, -130, 56 }, -40 },
};

// Trial state machine
#define GJ_IDLE  0
#define GJ_CROSS 1
#define GJ_COOL  2

static int   gj_state[MAX_CLIENTS];
static int   gj_lane_active[MAX_CLIENTS];
static int   gj_trial[MAX_CLIENTS];
static float gj_t0[MAX_CLIENTS];
static float gj_cool_t0[MAX_CLIENTS];
static float gj_peak[MAX_CLIENTS];
static float gj_flip[MAX_CLIENTS];
static qbool gj_jump_latch[MAX_CLIENTS];
static qbool gj_has_flown[MAX_CLIENTS];
static float gj_probe_log[MAX_CLIENTS];

// Resolve the active lane geometry, honouring cvar overrides (retune without a
// rebuild). k_kbot_gj_to / _land are "x y z" strings; empty -> table value.
static void GJ_Geometry(int lane, vec3_t takeoff, vec3_t landing, float *fail_z)
{
	char buf[64];
	float x, y, z;

	VectorCopy(gj_lanes[lane].takeoff, takeoff);
	VectorCopy(gj_lanes[lane].landing, landing);
	*fail_z = gj_lanes[lane].fail_z;

	trap_cvar_string("k_kbot_gj_to", buf, sizeof(buf));
	if (buf[0] && sscanf(buf, "%f %f %f", &x, &y, &z) == 3)
	{
		VectorSet(takeoff, x, y, z);
	}
	trap_cvar_string("k_kbot_gj_land", buf, sizeof(buf));
	if (buf[0] && sscanf(buf, "%f %f %f", &x, &y, &z) == 3)
	{
		VectorSet(landing, x, y, z);
	}
	if (cvar("k_kbot_gj_failz") != 0)
	{
		*fail_z = cvar("k_kbot_gj_failz");
	}
}

// Bearing (yaw degrees) from -> to, horizontal only.
static float GJ_Bearing(vec3_t from, vec3_t to)
{
	return atan2(to[1] - from[1], to[0] - from[0]) * 180.0f / M_PI;
}

// Teleport the bot to the lane takeoff (optionally backed off along -bearing by
// k_kbot_gj_runup) and seed horizontal run speed k_kbot_gj_v0 aimed at landing.
static void GJ_Seat(gedict_t *self, int lane)
{
	vec3_t takeoff, landing, org, fwd, ang = { 0, 0, 0 };
	float fail_z, bearing, v0, runup;

	GJ_Geometry(lane, takeoff, landing, &fail_z);
	bearing = GJ_Bearing(takeoff, landing);
	if (cvar("k_kbot_gj_head") > -360)
	{
		bearing = cvar("k_kbot_gj_head");
	}
	v0 = cvar("k_kbot_gj_v0");
	if (v0 <= 0)
	{
		v0 = 450;
	}
	runup = cvar("k_kbot_gj_runup");

	ang[YAW] = bearing;
	trap_makevectors(ang);
	VectorCopy(g_globalvars.v_forward, fwd);
	fwd[2] = 0;
	VectorNormalize(fwd);

	VectorCopy(takeoff, org);
	// Back the spawn off toward the takeoff side so the bot runs onto the lip.
	org[0] -= fwd[0] * runup;
	org[1] -= fwd[1] * runup;
	setorigin(self, PASSVEC3(org));

	// Seed grounded at the ledge so the frame-perfect launch hop fires on the
	// very first CROSS frame (at the edge) -- without this, a runup-0 seed is
	// airborne and runs off the lip WITHOUT jumping (pure ballistic drop).
	self->s.v.flags = (int)self->s.v.flags | FL_ONGROUND;

	VectorScale(fwd, v0, self->s.v.velocity);
	self->s.v.velocity[2] = 0;
}

// Execute one crossing frame. Returns true (owns the command).
static qbool GJ_Cross(gedict_t *self, int slot, int lane, qbool *jumping,
					  qbool *firing, int *impulse, vec3_t direction)
{
	vec3_t takeoff, landing, cur, wish, ang, org;
	float fail_z, bearing, err, deadband, speed, hdist, timeout, landrad, view_yaw;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	float now = g_globalvars.time;
	qbool press;

	GJ_Geometry(lane, takeoff, landing, &fail_z);
	VectorCopy(self->s.v.origin, org);

	cur[0] = self->s.v.velocity[0];
	cur[1] = self->s.v.velocity[1];
	cur[2] = 0;
	speed = VectorLength(cur);
	if (speed > gj_peak[slot])
	{
		gj_peak[slot] = speed;
	}

	bearing = GJ_Bearing(org, landing);
	deadband = cvar("k_kbot_gj_steer");
	if (deadband <= 0)
	{
		deadband = 5;
	}

	if (onground)
	{
		// On the runway: aim straight at the landing, hop (friction-free).
		vec3_t bang = { 0, 0, 0 };

		bang[YAW] = bearing;
		trap_makevectors(bang);
		VectorCopy(g_globalvars.v_forward, wish);
		wish[2] = 0;
		VectorNormalize(wish);
	}
	else if (speed < 1 || VectorNormalize(cur) <= 0)
	{
		vec3_t bang = { 0, 0, 0 };

		bang[YAW] = bearing;
		trap_makevectors(bang);
		VectorCopy(g_globalvars.v_forward, wish);
		wish[2] = 0;
		VectorNormalize(wish);
	}
	else
	{
		// Airborne carve-steer toward the landing bearing. err in (-180,180].
		vec3_t up = { 0, 0, 1 };
		float vyaw = atan2(cur[1], cur[0]) * 180.0f / M_PI;

		err = bearing - vyaw;
		while (err > 180)
		{
			err -= 360;
		}
		while (err < -180)
		{
			err += 360;
		}

		if (err > deadband)
		{
			// need to rotate velocity CCW (+yaw): wishdir = v rotated +90.
			RotatePointAroundVector(wish, up, cur, 90.0f);
		}
		else if (err < -deadband)
		{
			RotatePointAroundVector(wish, up, cur, -90.0f);
		}
		else
		{
			// on-bearing: alternate for zero net rotation (straight, keep speed)
			RotatePointAroundVector(wish, up, cur, 90.0f * gj_flip[slot]);
			gj_flip[slot] = -gj_flip[slot];
		}
		wish[2] = 0;
		VectorNormalize(wish);
	}

	// Frame-perfect hop latch (E1): press only on a grounded, released frame.
	press = onground && !gj_jump_latch[slot];
	gj_jump_latch[slot] = press;

	if (!onground)
	{
		gj_has_flown[slot] = true;
	}

	// Projection seam: wishdir -> fmove/smove through the view yaw (cancels).
	view_yaw = bearing;
	VectorSet(ang, 0, view_yaw, 0);
	trap_makevectors(ang);
	self->fb.desired_angle[PITCH] = 0;
	self->fb.desired_angle[YAW] = view_yaw;
	self->fb.desired_angle[ROLL] = 0;
	direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
	direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
	direction[2] = 0;
	*jumping = press;
	*firing = false;
	*impulse = 0;

	if (cvar("k_kbot_gj_traj"))
	{
		G_cprint("[gjtraj] t=%.3f pos=%.0f,%.0f,%.0f vel=%.0f,%.0f,%.0f spd=%.0f og=%d press=%d bear=%.0f\n",
				 now - gj_t0[slot], org[0], org[1], org[2],
				 self->s.v.velocity[0], self->s.v.velocity[1], self->s.v.velocity[2],
				 speed, onground ? 1 : 0, press ? 1 : 0, bearing);
	}

	// ---- outcome detection ----
	timeout = cvar("k_kbot_gj_timeout");
	if (timeout <= 0)
	{
		timeout = 4;
	}
	landrad = cvar("k_kbot_gj_landrad");
	if (landrad <= 0)
	{
		landrad = 64;
	}
	hdist = sqrt((org[0] - landing[0]) * (org[0] - landing[0]) +
				 (org[1] - landing[1]) * (org[1] - landing[1]));

	if (gj_has_flown[slot] && onground && hdist < landrad &&
		fabs(org[2] - landing[2]) < 64)
	{
		G_cprint("[gapjump] lane=%s trial=%d result=LAND land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot]);
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}
	if (org[2] < fail_z)
	{
		G_cprint("[gapjump] lane=%s trial=%d result=FAIL_GAP land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot]);
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}
	if ((now - gj_t0[slot]) > timeout)
	{
		G_cprint("[gapjump] lane=%s trial=%d result=FAIL_TIMEOUT land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot]);
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}

	return true;
}

// Start a fresh trial for the given lane.
static void GJ_StartTrial(gedict_t *self, int slot, int lane)
{
	gj_lane_active[slot] = lane;
	gj_trial[slot]++;
	gj_t0[slot] = g_globalvars.time;
	gj_peak[slot] = 0;
	gj_flip[slot] = 1;
	gj_jump_latch[slot] = false;
	gj_has_flown[slot] = false;
	gj_state[slot] = GJ_CROSS;
	GJ_Seat(self, lane);
}

qbool KBot_GapjumpFrame(gedict_t *self, qbool *jumping, qbool *firing,
					   int *impulse, vec3_t direction)
{
	int slot = NUM_FOR_EDICT(self) - 1;
	int lane;
	float now = g_globalvars.time;
	float cool;

	if (!cvar("k_kbot_gapjump") || slot < 0 || slot >= MAX_CLIENTS)
	{
		return false; // neutral-off: byte-for-byte vanilla
	}

	// Geometry probe: log origin + loc for a vanilla-navigating kbot so the
	// lane table can be pinned in server coordinates. Inert (vanilla nav runs).
	if (cvar("k_kbot_gj_probe") && (int)cvar("k_kbot_gj_lane") < 0)
	{
		if (!ISDEAD(self) && ((now - gj_probe_log[slot]) >= 0.2f || gj_probe_log[slot] > now))
		{
			vec3_t v;
			float sp;

			gj_probe_log[slot] = now;
			v[0] = self->s.v.velocity[0];
			v[1] = self->s.v.velocity[1];
			v[2] = 0;
			sp = VectorLength(v);
			G_cprint("[gjprobe] t=%.2f pos=%.0f,%.0f,%.0f og=%d spd=%.0f loc=%s\n",
					 now, self->s.v.origin[0], self->s.v.origin[1],
					 self->s.v.origin[2],
					 ((int)self->s.v.flags & FL_ONGROUND) ? 1 : 0, sp,
					 LocationName(PASSVEC3(self->s.v.origin)));
		}
		return false;
	}

	// ---- TRACE CALIBRATION: downward traceline from k_kbot_gj_to to map the
	// true floor height / gap at any XY without dropping (or killing) the bot.
	// floorz near a ledge height (56/88) + nz~1 => solid ledge; floorz far
	// below => central pit/gap. Sweep k_kbot_gj_to from the console.
	if (cvar("k_kbot_gj_cal"))
	{
		char buf[64];
		float x, y, z;

		if ((now - gj_probe_log[slot]) >= 0.25f)
		{
			gj_probe_log[slot] = now;
			trap_cvar_string("k_kbot_gj_to", buf, sizeof(buf));
			if (buf[0] && sscanf(buf, "%f %f %f", &x, &y, &z) == 3)
			{
				vec3_t top, bot;
				int dir = (int)cvar("k_kbot_gj_caldir");
				float d = 700;

				VectorSet(top, x, y, z);
				if (dir == 1)       { VectorSet(bot, x + d, y, z); }   // +X
				else if (dir == 2)  { VectorSet(bot, x - d, y, z); }   // -X
				else if (dir == 3)  { VectorSet(bot, x, y + d, z); }   // +Y
				else if (dir == 4)  { VectorSet(bot, x, y - d, z); }   // -Y
				else                { VectorSet(bot, x, y, z - 1400); } // down
				traceline(PASSVEC3(top), PASSVEC3(bot), true, self);
				G_cprint("[gjcal] to=(%s) dir=%d hit=%.0f,%.0f,%.0f nz=%.2f frac=%.3f loc=%s\n",
						 buf, dir, g_globalvars.trace_endpos[0],
						 g_globalvars.trace_endpos[1], g_globalvars.trace_endpos[2],
						 g_globalvars.trace_plane_normal[2],
						 g_globalvars.trace_fraction,
						 LocationName(PASSVEC3(g_globalvars.trace_endpos)));
			}
		}
		VectorClear(direction);
		*jumping = false;
		*firing = false;
		*impulse = 0;
		return true;
	}

	lane = (int)cvar("k_kbot_gj_lane");

	// ---- TRIAL DRIVER: k_kbot_gj_lane in [0..3] ----
	if (lane >= 0 && lane < GJ_NUM_LANES)
	{
		if (ISDEAD(self))
		{
			return false;
		}
		cool = cvar("k_kbot_gj_cool");
		if (cool <= 0)
		{
			cool = 0.6f;
		}

		// (Re)start when idle or when the selected lane changed.
		if (gj_state[slot] == GJ_IDLE || gj_lane_active[slot] != lane)
		{
			gj_trial[slot] = 0;
			GJ_StartTrial(self, slot, lane);
			G_cprint("[gapjump] init lane=%s\n", gj_lanes[lane].name);
		}

		if (gj_state[slot] == GJ_COOL)
		{
			if ((now - gj_cool_t0[slot]) >= cool || gj_cool_t0[slot] > now)
			{
				GJ_StartTrial(self, slot, lane);
			}
			else
			{
				// Freeze in place during cooldown (no vanilla drift/heartbeat).
				VectorClear(direction);
				*jumping = false;
				*firing = false;
				*impulse = 0;
				VectorClear(self->s.v.velocity);
				return true;
			}
		}

		return GJ_Cross(self, slot, lane, jumping, firing, impulse, direction);
	}

	// ---- PASSIVE TRIGGER (real feature): lane < 0 ----
	// Continue an in-flight passive crossing across frames until it resolves.
	if (gj_state[slot] == GJ_CROSS)
	{
		if (ISDEAD(self))
		{
			gj_state[slot] = GJ_IDLE;
			return false;
		}
		return GJ_Cross(self, slot, gj_lane_active[slot], jumping, firing,
						impulse, direction);
	}
	if (gj_state[slot] == GJ_COOL)
	{
		gj_state[slot] = GJ_IDLE; // release movement back to vanilla nav
		return false;
	}

	// Combat-yield: never trigger with an enemy near (D5). Reuse the frogbot's
	// own enemy-visibility state -- if a live enemy is visible, let vanilla nav
	// and combat run.
	if (self->fb.enemy_visible || ISDEAD(self))
	{
		return false;
	}
	{
		// Find the takeoff lane whose zone contains us and whose landing is the
		// side our nav goal is on. Zone = within GJ_ZONE of a takeoff origin.
		float best = 1e30f, zone = cvar("k_kbot_gj_zone");
		int i, pick = -1;
		vec3_t org;

		if (zone <= 0)
		{
			zone = 96;
		}
		VectorCopy(self->s.v.origin, org);
		for (i = 0; i < GJ_NUM_LANES; i++)
		{
			vec3_t t;
			float d;

			VectorCopy(gj_lanes[i].takeoff, t);
			d = sqrt((org[0] - t[0]) * (org[0] - t[0]) +
					 (org[1] - t[1]) * (org[1] - t[1]) +
					 (org[2] - t[2]) * (org[2] - t[2]));
			if (d < zone && d < best)
			{
				best = d;
				pick = i;
			}
		}
		if (pick < 0)
		{
			return false; // not on a takeoff ledge
		}
		// Goal must be across the gap: the current nav target (linked_marker or
		// look_object) should be nearer the landing than the takeoff.
		{
			gedict_t *goal = self->fb.linked_marker ? self->fb.linked_marker
													: self->fb.look_object;
			vec3_t land, take;
			float dg_land, dg_take;

			if (!goal)
			{
				return false;
			}
			VectorCopy(gj_lanes[pick].landing, land);
			VectorCopy(gj_lanes[pick].takeoff, take);
			dg_land = VectorDistance(goal->s.v.origin, land);
			dg_take = VectorDistance(goal->s.v.origin, take);
			if (dg_land > dg_take)
			{
				return false; // goal is not across the gap
			}
			// Execute one crossing (no teleport; use live position/speed).
			if (gj_state[slot] != GJ_CROSS || gj_lane_active[slot] != pick)
			{
				gj_lane_active[slot] = pick;
				gj_t0[slot] = now;
				gj_peak[slot] = 0;
				gj_flip[slot] = 1;
				gj_jump_latch[slot] = false;
				gj_has_flown[slot] = false;
				gj_state[slot] = GJ_CROSS;
			}
			return GJ_Cross(self, slot, pick, jumping, firing, impulse, direction);
		}
	}
}

#endif // BOT_SUPPORT
