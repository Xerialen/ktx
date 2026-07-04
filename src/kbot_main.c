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
	float head_off; // per-lane launch-heading offset (deg) to bow around the pillar
} gj_lane_t;

// E8.1 GEOMETRY CORRECTION (coordinator, verified vs real human ring<->quad MVD
// jumps + floor traces). The earlier table used ITEM-to-ITEM x (360<->796,
// ~436u) at y=40/-130, but that whole corridor is OPEN VOID (floor -224 across
// x~370..765 => ~395u wide, which needs ~600 ups -- unreachable in match).
// Players do NOT jump item-to-item; they jump the pit VOID lip-to-lip at the
// NORTHERN corridor (y~146), where the ring/quad ledges have horns that pinch
// the gap to ~250u:
//   traced floor y=146: z=32 (ledge) for x<=450, z=-224 (pit) x=470..700,
//   z=32 (ledge) for x>=720  => near lip ~x=460, far lip ~x=710, span ~250u.
// A +270 self-jump clears ~250u at only ~344 ups (v_req), BELOW the ~440 the bot
// reaches in-match. But a STRAIGHT +X hop at y=146 smacks the central PILLAR
// (~x=496, tall) and pins the bot; humans bow SOUTH to apex y~77 to thread past
// it (biggz: 466,146 -> apex 565,77 -> 693,142). So each lane carries a launch
// HEADING OFFSET (bow toward the open corridor); the air-carve then brings the
// arc back to the far lip. Measured (v0=440, y=146 lips): straight (0 deg) hits
// the pillar (0% land); head_off -30 lands 17/17 (100%); reverse +30 lands 100%.
// y=-146 is all void (no southern horns), so there is only ONE crossable pit --
// lanes 2/3 mirror 0/1 so an RA/YA-context nav goal still uses the valid jump.
#define GJ_NUM_LANES 4
static const gj_lane_t gj_lanes[GJ_NUM_LANES] = {
	// 0: ring -> quad, northern lips, bow south (-30) around the pillar.
	{ "ring2quad", { 455, 146, 56 }, { 705, 146, 56 }, -40, -30 },
	// 1: quad -> ring, reverse (mirror bow, +30).
	{ "quad2ring", { 705, 146, 56 }, { 455, 146, 56 }, -40, +30 },
	// 2/3: RA<->YA share the one crossable pit (no southern narrow lane exists).
	{ "ra2ya", { 455, 146, 56 }, { 705, 146, 56 }, -40, -30 },
	{ "ya2ra", { 705, 146, 56 }, { 455, 146, 56 }, -40, +30 },
};

// Effective launch-heading offset for a lane: table default + cvar (for sweeps).
static float GJ_LaneHeadOff(int lane)
{
	return gj_lanes[lane].head_off + cvar("k_kbot_gj_head_off");
}

// E8.2 PILLAR-GAP AIR WAYPOINT. In-match the bot arrives with its NAV heading,
// so the fixed launch offset alone mis-bows and it misses the far-lip horn. The
// robust fix is to air-carve through an intermediate waypoint SOUTH of the
// central pillar (the human apex ~ (565,77) on the y=146 lane) BEFORE steering
// to the landing -- this bends the arc around the pillar and self-corrects in
// the air regardless of the launch heading. The waypoint is the lane midpoint
// bowed toward the open corridor (lower y) by k_kbot_gj_wp (0 = disabled, aim
// straight at the landing). Returns the point the carve should currently steer
// at: the waypoint until the bot passes the lane midpoint, then the landing.
static void GJ_CarveTarget(int lane, vec3_t takeoff, vec3_t landing, vec3_t org,
						   vec3_t target)
{
	float wp = cvar("k_kbot_gj_wp");
	vec3_t mid, dir;
	float lanelen, prog, wpprog;

	(void)lane;
	if (wp == 0)
	{
		VectorCopy(landing, target); // waypoint disabled
		return;
	}
	mid[0] = (takeoff[0] + landing[0]) * 0.5f;
	mid[1] = (takeoff[1] + landing[1]) * 0.5f - wp; // bow south (open corridor)
	mid[2] = (takeoff[2] + landing[2]) * 0.5f;

	dir[0] = landing[0] - takeoff[0];
	dir[1] = landing[1] - takeoff[1];
	dir[2] = 0;
	lanelen = VectorLength(dir);
	if (lanelen < 1)
	{
		VectorCopy(landing, target);
		return;
	}
	VectorNormalize(dir);
	prog = (org[0] - takeoff[0]) * dir[0] + (org[1] - takeoff[1]) * dir[1];
	wpprog = (mid[0] - takeoff[0]) * dir[0] + (mid[1] - takeoff[1]) * dir[1];
	if (prog < wpprog)
	{
		VectorCopy(mid, target);
	}
	else
	{
		VectorCopy(landing, target);
	}
}

// Trial state machine
#define GJ_IDLE  0
#define GJ_CROSS 1
#define GJ_COOL  2
#define GJ_BUILD 3   // E8: circle-jump run-up to reach v_req before launching
#define GJ_APPROACH 4 // E9: deliberate drive-to-lip + align + build, then launch

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
static float gj_build_t0[MAX_CLIENTS];   // E8 build-state start time
static int   gj_build_sign[MAX_CLIENTS]; // E8 circle-jump strafe sign
static float gj_app_t0[MAX_CLIENTS];     // E9 approach-state start time
static float gj_app_supp[MAX_CLIENTS];   // E9 re-engage suppress-until time

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

// ---------------------------------------------------------------------------
//  E8: ballistic launch model (heading + required-speed gate)
// ---------------------------------------------------------------------------
// The dm3 ring<->quad cross is a LEVEL ~430u self-jump over a 256-deep pit.
// A self-jump adds a FIXED vz=+270 (sv_gravity 800), so the airborne arc
// returns to launch height after T = 2*vz/g = 0.675 s (dz=0). To land ON the
// far ledge the horizontal speed must carry the bot the full gap D within T:
//
//     v_req = D / T,   T = (vz + sqrt(vz*vz - 2*g*dz)) / g
//
// (descending root; dz = landing_z - takeoff_z). Measured in the isolated
// trial harness (E8): launch 475 -> peak 530 lands 0/13; launch 600 -> peak
// 637 lands 11/16 (69%); launch 640 -> 94%. Air-strafe during the arc adds
// ~5-8% over the launch speed, so the LAUNCH threshold sits a little below
// v_req: k_kbot_gj_airgain (default 0.93) models that discount.
//
// KEY E8 FINDING (telemetry-backed): at 475 the arc already lands hdist~25
// LATERALLY on target -- the miss is purely VERTICAL (arrives ~100u too low).
// Sweeping the launch heading -30..+11 deg at v0=475 lands 0% at every angle.
// So on dm3 the jump is won by SPEED, not heading (the human -11 deg result
// was on ztricks' shorter/downhill "Distance" gap, not this level 430u pit).
// The launch heading is still computed ballistically (bearing + per-lane
// offset cvar) for lateral precision, but the decisive lever is the
// REQUIRED-SPEED GATE: the passive trigger only commits a crossing when the
// approach speed can actually clear the gap -- otherwise it declines, so the
// bot stops throwing itself into the pit (E7's -9.92 came from 126 pit falls).
#define GJ_SELFJUMP_VZ 270.0f

static float GJ_Gravity(void)
{
	float g = cvar("sv_gravity");
	return (g > 0) ? g : 800.0f;
}

// Airborne time until the +vz self-jump arc returns to landing height.
static float GJ_AirTime(float dz)
{
	float g = GJ_Gravity();
	float vz = GJ_SELFJUMP_VZ;
	float disc = vz * vz - 2.0f * g * dz;

	if (disc < 0)
	{
		disc = 0; // landing higher than the arc peak reaches; clamp
	}
	return (vz + sqrt(disc)) / g;
}

// Minimum horizontal launch speed to land the lane's ballistic arc. Honours a
// direct override (k_kbot_gj_vreq) for sweeps; else D/T with the air-accel
// discount k_kbot_gj_airgain.
static float GJ_RequiredSpeed(vec3_t takeoff, vec3_t landing)
{
	float over = cvar("k_kbot_gj_vreq");
	float dx = landing[0] - takeoff[0];
	float dy = landing[1] - takeoff[1];
	float dz = landing[2] - takeoff[2];
	float D = sqrt(dx * dx + dy * dy);
	float T, gain;

	if (over > 0)
	{
		return over;
	}
	T = GJ_AirTime(dz);
	if (T < 0.01f)
	{
		T = 0.01f;
	}
	gain = cvar("k_kbot_gj_airgain");
	if (gain <= 0 || gain > 1.0f)
	{
		gain = 0.93f;
	}
	return (D / T) * gain;
}

// Teleport the bot to the lane takeoff (optionally backed off along -bearing by
// k_kbot_gj_runup) and seed horizontal run speed k_kbot_gj_v0 aimed at landing.
static void GJ_Seat(gedict_t *self, int lane)
{
	vec3_t takeoff, landing, org, fwd, ang = { 0, 0, 0 };
	float fail_z, bearing, v0, runup;

	GJ_Geometry(lane, takeoff, landing, &fail_z);
	// Ballistic launch heading: aim at the far ledge, plus a per-lane offset
	// (the human -11 deg lever; default 0 -- E8 found straight aim already
	// lands laterally on dm3). k_kbot_gj_head is an absolute override for sweeps.
	bearing = GJ_Bearing(takeoff, landing) + GJ_LaneHeadOff(lane);
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
	float fail_z, bearing, launch_bearing, err, deadband, speed, hdist, timeout, landrad, view_yaw, vreq;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	float now = g_globalvars.time;
	qbool press;

	GJ_Geometry(lane, takeoff, landing, &fail_z);
	VectorCopy(self->s.v.origin, org);

	// Fixed launch heading (ballistic aim at the far ledge + per-lane offset);
	// used on the ground/launch frame so the hop leaves at the lane heading.
	launch_bearing = GJ_Bearing(takeoff, landing) + GJ_LaneHeadOff(lane);
	if (cvar("k_kbot_gj_head") > -360)
	{
		launch_bearing = cvar("k_kbot_gj_head");
	}
	vreq = GJ_RequiredSpeed(takeoff, landing);

	cur[0] = self->s.v.velocity[0];
	cur[1] = self->s.v.velocity[1];
	cur[2] = 0;
	speed = VectorLength(cur);
	if (speed > gj_peak[slot])
	{
		gj_peak[slot] = speed;
	}

	// Air-carve target: the pillar-gap waypoint (bow south) until mid-span, then
	// the landing. Self-corrects the arc around the pillar regardless of the
	// launch heading (E8.2). k_kbot_gj_wp 0 disables it (aim straight at landing).
	{
		vec3_t ctarget;

		GJ_CarveTarget(lane, takeoff, landing, org, ctarget);
		bearing = GJ_Bearing(org, ctarget);
	}
	deadband = cvar("k_kbot_gj_steer");
	if (deadband <= 0)
	{
		deadband = 5;
	}

	if (onground)
	{
		// On the runway: aim at the fixed LAUNCH heading (ballistic aim + lane
		// offset), hop (friction-free). Using launch_bearing (not the live
		// org->landing bearing) keeps the takeoff direction precise.
		vec3_t bang = { 0, 0, 0 };

		bang[YAW] = launch_bearing;
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

	// E8.2 diagnostic: at the hop frame, log the actual launch VELOCITY heading
	// vs the intended launch_bearing -- the in-match mismatch (bot arrives with
	// its nav heading) is exactly this gap. gated on k_kbot_gj_gatelog.
	if (press && cvar("k_kbot_gj_gatelog"))
	{
		float lvyaw = (speed > 1) ? atan2(self->s.v.velocity[1],
										   self->s.v.velocity[0]) * 180.0f / M_PI
								  : launch_bearing;
		float lerr = launch_bearing - lvyaw;

		while (lerr > 180) { lerr -= 360; }
		while (lerr < -180) { lerr += 360; }
		G_cprint("[gjlaunch] lane=%s vyaw=%.0f want=%.0f err=%.0f speed=%.0f "
				 "pos=%.0f,%.0f\n",
				 gj_lanes[lane].name, lvyaw, launch_bearing, lerr, speed,
				 org[0], org[1]);
	}

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
				 "hdist=%.0f peak_speed=%.0f tair=%.2f vreq=%.0f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot], vreq);
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}
	if (org[2] < fail_z)
	{
		G_cprint("[gapjump] lane=%s trial=%d result=FAIL_GAP land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f vreq=%.0f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot], vreq);
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}
	if ((now - gj_t0[slot]) > timeout)
	{
		G_cprint("[gapjump] lane=%s trial=%d result=FAIL_TIMEOUT land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f vreq=%.0f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot], vreq);
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

// E8: grounded circle-jump run-up (experimental, k_kbot_gj_build > 0). While
// too slow, hold the wishdir k_kbot_gj_build_angle deg off the velocity with
// full forwardmove and the jump suppressed -- QW ground accelerate keeps adding
// speed past maxspeed while |v|.wishdir < maxspeed. Release into GJ_CROSS (the
// hop fires there) once vh >= v_req. Abort -> decline on timeout or if pushed
// airborne still under-speed, so the bot never launches a doomed jump.
static qbool GJ_BuildFrame(gedict_t *self, int slot, int lane, qbool *jumping,
						   qbool *firing, int *impulse, vec3_t direction)
{
	vec3_t takeoff, landing, cur, wish, ang, up = { 0, 0, 1 };
	float fail_z, vreq, gate, vh, launch_bearing, angle, view_yaw, timeout;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	float now = g_globalvars.time;

	GJ_Geometry(lane, takeoff, landing, &fail_z);
	vreq = GJ_RequiredSpeed(takeoff, landing);
	gate = cvar("k_kbot_gj_gate");
	if (gate <= 0)
	{
		gate = 0.98f;
	}
	launch_bearing = GJ_Bearing(takeoff, landing) + GJ_LaneHeadOff(lane);
	if (cvar("k_kbot_gj_head") > -360)
	{
		launch_bearing = cvar("k_kbot_gj_head");
	}

	if (gj_state[slot] != GJ_BUILD)
	{
		gj_state[slot] = GJ_BUILD;
		gj_lane_active[slot] = lane;
		gj_build_t0[slot] = now;
		gj_build_sign[slot] = 0;
		gj_jump_latch[slot] = false;
	}

	cur[0] = self->s.v.velocity[0];
	cur[1] = self->s.v.velocity[1];
	cur[2] = 0;
	vh = VectorLength(cur);

	timeout = cvar("k_kbot_gj_buildtime");
	if (timeout <= 0)
	{
		timeout = 1.5f;
	}

	// Abort -> decline (never launch a doomed jump).
	if ((now - gj_build_t0[slot]) > timeout || (!onground && vh < vreq * gate))
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=BUILD_ABORT vh=%.0f vreq=%.0f og=%d\n",
					 gj_lanes[lane].name, vh, vreq, onground ? 1 : 0);
		}
		gj_state[slot] = GJ_IDLE;
		return false;
	}

	// Fast enough while grounded -> release to CROSS (the hop fires there).
	if (onground && vh >= vreq * gate)
	{
		gj_t0[slot] = now;
		gj_peak[slot] = 0;
		gj_flip[slot] = 1;
		gj_jump_latch[slot] = false;
		gj_has_flown[slot] = false;
		gj_state[slot] = GJ_CROSS;
		return GJ_Cross(self, slot, lane, jumping, firing, impulse, direction);
	}

	// The grounded circle: wishdir = velocity rotated launch_angle*sign toward
	// the launch heading; view aims along wishdir, full forward, jump suppressed.
	angle = cvar("k_kbot_gj_build_angle");
	if (angle <= 0)
	{
		angle = 42.0f;
	}
	if (gj_build_sign[slot] == 0)
	{
		float vyaw = (vh > 1) ? atan2(cur[1], cur[0]) * 180.0f / M_PI : launch_bearing;
		float e = launch_bearing - vyaw;

		while (e > 180)
		{
			e -= 360;
		}
		while (e < -180)
		{
			e += 360;
		}
		gj_build_sign[slot] = (e >= 0) ? 1 : -1;
	}
	if (vh > 1)
	{
		VectorNormalize(cur);
		RotatePointAroundVector(wish, up, cur, angle * gj_build_sign[slot]);
	}
	else
	{
		vec3_t bang = { 0, 0, 0 };

		bang[YAW] = launch_bearing;
		trap_makevectors(bang);
		VectorCopy(g_globalvars.v_forward, wish);
	}
	wish[2] = 0;
	VectorNormalize(wish);

	view_yaw = vectoyaw(wish);
	VectorSet(ang, 0, view_yaw, 0);
	trap_makevectors(ang);
	self->fb.desired_angle[PITCH] = 0;
	self->fb.desired_angle[YAW] = view_yaw;
	self->fb.desired_angle[ROLL] = 0;
	direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
	direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
	direction[2] = 0;
	*jumping = false;
	*firing = false;
	*impulse = 0;
	return true;
}

// ---------------------------------------------------------------------------
//  E9: ACTIVE jump-intent (nav integration)
// ---------------------------------------------------------------------------
// The E8.2 passive trigger only fires when nav INCIDENTALLY passes the takeoff
// lip already aligned + fast -- rare (~1.6 attempts/match) and low yield (~31%
// land). E9 makes the launch conditions TRUE BY CONSTRUCTION: when a kbot's nav
// GOAL is on the far side of a gap-lane and the bot is on the takeoff side (no
// enemy near), it DELIBERATELY drives to the lip, converges onto the launch
// ray, builds to v_req if slow, and hands off to the proven E8.2 crossing when
// it arrives at the lip aligned + fast. If it cannot complete (blocked, timeout,
// enemy) it aborts to vanilla nav -- it never launches a doomed jump, and the
// existing pit-fall gates remain the final safety.

// Resolve the bot's NAV GOAL entity: the item it is routing to (goalentity),
// else the current path/aim marker. NULL if none.
static gedict_t *GJ_GoalEntity(gedict_t *self)
{
	gedict_t *goal = NULL;
	int gn = (int)self->s.v.goalentity;

	if (gn > 0 && gn < MAX_EDICTS)
	{
		goal = &g_edicts[gn];
	}
	if (!goal || goal == world)
	{
		goal = self->fb.linked_marker ? self->fb.linked_marker : self->fb.look_object;
	}
	if (!goal || goal == world)
	{
		return NULL;
	}
	return goal;
}

// Pick the gap-lane the bot should deliberately set up: bot on the takeoff side
// (behind the lip along the lane, within the corridor band, on the ledge) AND
// the GOAL is across the gap (projects past the lane midpoint toward landing).
// Returns lane index, or -1. Nearest-to-lip wins.
static int GJ_PickIntentLane(gedict_t *self)
{
	int i, pick = -1;
	float best = 1e30f;
	vec3_t org, goalorg;
	gedict_t *goal;
	float back = cvar("k_kbot_gj_intent_back");
	float pmax = cvar("k_kbot_gj_intent_perp");
	float zband = cvar("k_kbot_gj_intent_zband");

	if (back <= 0)  { back = 384; }
	if (pmax <= 0)  { pmax = 176; }
	if (zband <= 0) { zband = 56; }

	goal = GJ_GoalEntity(self);
	if (!goal)
	{
		return -1;
	}
	VectorCopy(goal->s.v.origin, goalorg);
	VectorCopy(self->s.v.origin, org);

	for (i = 0; i < GJ_NUM_LANES; i++)
	{
		vec3_t take, land, u, perp, rel;
		float lanelen, along, lat, galong;

		VectorCopy(gj_lanes[i].takeoff, take);
		VectorCopy(gj_lanes[i].landing, land);
		u[0] = land[0] - take[0];
		u[1] = land[1] - take[1];
		u[2] = 0;
		lanelen = VectorLength(u);
		if (lanelen < 1)
		{
			continue;
		}
		VectorNormalize(u);
		perp[0] = -u[1];
		perp[1] = u[0];
		perp[2] = 0;
		rel[0] = org[0] - take[0];
		rel[1] = org[1] - take[1];
		rel[2] = 0;
		along = DotProduct(rel, u);
		lat = rel[0] * perp[0] + rel[1] * perp[1];

		if (along > 32 || along < -back)   // must be on the takeoff side
		{
			continue;
		}
		if (fabs(lat) > pmax)              // within the approach corridor
		{
			continue;
		}
		if (fabs(org[2] - take[2]) > zband) // on the takeoff ledge
		{
			continue;
		}
		galong = (goalorg[0] - take[0]) * u[0] + (goalorg[1] - take[1]) * u[1];
		if (galong < lanelen * 0.5f)       // goal must be across the gap
		{
			continue;
		}
		if (fabs(along) < best)
		{
			best = fabs(along);
			pick = i;
		}
	}
	return pick;
}

// One deliberate-approach frame. Drives the bot onto the launch ray toward the
// lip, builds to v_req when slow, and commits the E8.2 crossing at the lip.
// Returns true (owns the command) while approaching; on abort sets IDLE and
// returns false so vanilla nav resumes.
static qbool GJ_ApproachFrame(gedict_t *self, int slot, int lane, qbool *jumping,
							  qbool *firing, int *impulse, vec3_t direction)
{
	vec3_t take, land, org, u, rel, tgt, wish, ang, up = { 0, 0, 1 };
	float fail_z, vreq, lanelen, view_yaw, u_yaw;
	float along_u, lat_n, vh, floor, now, apptime, vyaw, aerr;
	float launch_win, launch_perp, lookahead, build_angle, ca, align_tol;
	qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
	qbool at_lip, fast, aligned;

	GJ_Geometry(lane, take, land, &fail_z);
	vreq = GJ_RequiredSpeed(take, land);
	now = g_globalvars.time;
	VectorCopy(self->s.v.origin, org);

	// Corridor axis u (lip -> landing, the wall-free y~146 line). We drive the
	// bot ALONG the corridor, not along the launch bow: the bow (-30 deg) points
	// partly off the ledge into the void, so nosing toward it stalls the bot at
	// the edge (measured: it reached the lip at vh~12-96 and timed out). Driving
	// along u keeps the bot on the ledge building speed; GJ_Cross applies the bow
	// on the hop frame and the air-carve corrects the arc.
	u[0] = land[0] - take[0];
	u[1] = land[1] - take[1];
	u[2] = 0;
	lanelen = VectorLength(u);
	if (lanelen < 1)
	{
		gj_state[slot] = GJ_IDLE;
		return false;
	}
	VectorNormalize(u);
	u_yaw = vectoyaw(u);

	rel[0] = org[0] - take[0];
	rel[1] = org[1] - take[1];
	rel[2] = 0;
	along_u = DotProduct(rel, u);
	// Lateral offset in WORLD +Y = NORTH (toward the central pillar) for BOTH lanes
	// -- the corridor runs along world X at y~146 and the pillar is always north of
	// it. Using u's perpendicular would flip sign between ring2quad and quad2ring
	// (quad2ring's -X axis makes uperp point south), which drove quad2ring INTO the
	// pillar. lat_n > 0 = north (pillar, reject); lat_n < 0 = south (open, allow).
	lat_n = org[1] - take[1];

	{
		vec3_t hv = { self->s.v.velocity[0], self->s.v.velocity[1], 0 };

		vh = VectorLength(hv);
	}
	apptime = cvar("k_kbot_gj_apptime");
	if (apptime <= 0) { apptime = 3.5f; }
	launch_win = cvar("k_kbot_gj_launch_win");
	if (launch_win <= 0) { launch_win = 48; }
	launch_perp = cvar("k_kbot_gj_launch_perp");
	if (launch_perp <= 0) { launch_perp = 28; }
	lookahead = cvar("k_kbot_gj_lookahead");
	if (lookahead <= 0) { lookahead = 112; }
	align_tol = cvar("k_kbot_gj_app_align");
	if (align_tol <= 0) { align_tol = 45; }
	{
		float cool = cvar("k_kbot_gj_app_cool");

		gj_app_supp[slot] = now + ((cool > 0) ? cool : 0.3f); // decline re-engage guard
	}
	// Launch SPEED FLOOR = v_req * launch_mul. v_req (~344) is the bare ballistic
	// minimum, but the air-carve scrubs speed while correcting heading, so a bot
	// launching AT v_req lands short in the pit (measured: 344 -> falls mid-gap;
	// the clean lands all left at 418-453). Require the margin; app_build tops the
	// bot up. Launching below the floor DECLINES rather than dives.
	{
		float mul = cvar("k_kbot_gj_launch_mul");

		if (mul <= 0) { mul = 1.2f; }
		floor = vreq * mul;
	}
	fast = vh >= floor;

	// Velocity heading error vs the corridor axis (the direction we deliver).
	vyaw = (vh > 1) ? atan2(self->s.v.velocity[1], self->s.v.velocity[0])
						  * 180.0f / M_PI
					: u_yaw;
	aerr = u_yaw - vyaw;
	while (aerr > 180) { aerr -= 360; }
	while (aerr < -180) { aerr += 360; }
	if (aerr < 0) { aerr = -aerr; }
	aligned = (aerr <= align_tol);

	// Abort -> vanilla nav (never launch a doomed jump).
	if ((now - gj_app_t0[slot]) > apptime)
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_ABORT_TIMEOUT along=%.0f lat=%.0f "
					 "vh=%.0f floor=%.0f\n",
					 gj_lanes[lane].name, along_u, lat_n, vh, floor);
		}
		gj_state[slot] = GJ_IDLE;
		return false;
	}
	if (self->fb.enemy_visible)   // enemy showed up mid-setup -> yield
	{
		gj_state[slot] = GJ_IDLE;
		return false;
	}

	// LAUNCH: grounded at the lip window (on the corridor line), fast enough AND
	// aligned -> commit the proven E8.2 crossing (hop fires on its first grounded
	// frame; GJ_Cross applies the launch bow + air-carve).
	// ASYMMETRIC perp gate: the corridor line (y~146) is the NORTHERN edge, and the
	// central pillar sits just north of it. Launches from NORTH of the line clip
	// the pillar and fall (measured: lat=+15/+18 -> FAIL_GAP; every LAND had
	// lat<=0). So allow the bot to sit SOUTH (into the open corridor) but reject
	// north-of-line launches: -launch_perp <= lat_n <= north_max.
	{
		float north_max = cvar("k_kbot_gj_north_max");

		if (north_max <= 0) { north_max = 12; }
		at_lip = (along_u >= -launch_win && along_u <= launch_win) &&
				 (lat_n >= -launch_perp && lat_n <= north_max) && aligned;
	}
	if (onground && at_lip && fast)
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_LAUNCH along=%.0f lat=%.0f vh=%.0f "
					 "vreq=%.0f floor=%.0f vyaw=%.0f uyaw=%.0f t=%.2f\n",
					 gj_lanes[lane].name, along_u, lat_n, vh, vreq, floor, vyaw,
					 u_yaw, now - gj_app_t0[slot]);
		}
		gj_t0[slot] = now;
		gj_peak[slot] = 0;
		gj_flip[slot] = 1;
		gj_jump_latch[slot] = false;
		gj_has_flown[slot] = false;
		gj_state[slot] = GJ_CROSS;
		return GJ_Cross(self, slot, lane, jumping, firing, impulse, direction);
	}

	// PAST THE LIP without launching (fast but mis-aligned, or drifted past the
	// window): decline immediately so we never steer a non-committed bot further
	// toward the gap. The ca<=0 carrot never drives here deliberately; this only
	// catches momentum overshoot.
	if (onground && along_u > launch_win)
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_DECLINE_PAST along=%.0f lat=%.0f "
					 "vh=%.0f aerr=%.0f\n",
					 gj_lanes[lane].name, along_u, lat_n, vh, aerr);
		}
		gj_state[slot] = GJ_IDLE;
		return false;
	}

	// STALLED AT THE LIP EDGE: reached the lip (along_u >= 0) grounded but not fast
	// enough to launch. The bot cannot walk off the ledge, so it just decelerates
	// here -- decline NOW (rather than burn the whole timeout) so vanilla nav
	// re-carries it and it re-approaches with a running start. Behind the lip
	// (along_u < 0) it still has runway to build, so we do NOT decline there.
	if (onground && (along_u >= 0) && !fast)
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_DECLINE_SLOW along=%.0f lat=%.0f "
					 "vh=%.0f floor=%.0f\n",
					 gj_lanes[lane].name, along_u, lat_n, vh, floor);
		}
		gj_state[slot] = GJ_IDLE;
		return false;
	}

	// Drive carrot: a point on the corridor line, ahead toward the lip but never
	// past it (ca clamped <= 0 so we never steer into the gap), and biased SOUTH of
	// the line (into the open corridor, away from the pillar) by south_bias so the
	// bot arrives south-of-line where the crossing is clear. Steering at this point
	// converges the bot onto the (biased) corridor AND forward to the lip.
	{
		float south_bias = cvar("k_kbot_gj_south_bias");

		if (south_bias < 0) { south_bias = 0; }
		ca = along_u + lookahead;
		if (ca > 0) { ca = 0; }
		tgt[0] = take[0] + u[0] * ca;
		tgt[1] = take[1] + u[1] * ca - south_bias; // bias toward world -Y (south)
		tgt[2] = org[2];
	}
	wish[0] = tgt[0] - org[0];
	wish[1] = tgt[1] - org[1];
	wish[2] = 0;
	if (VectorNormalize(wish) <= 0)
	{
		VectorCopy(u, wish);
	}

	// Under-speed with runway behind the lip -> circle-accel toward the drive
	// heading to build speed along the corridor (v_req ~344; nav momentum often
	// already meets the floor, so this fires mainly on slow arrivals).
	build_angle = cvar("k_kbot_gj_build_angle");
	if (build_angle <= 0) { build_angle = 42; }
	if (cvar("k_kbot_gj_app_build") && onground && !fast && along_u < 0)
	{
		vec3_t cur = { self->s.v.velocity[0], self->s.v.velocity[1], 0 };

		if (VectorLength(cur) > 40)   // need real velocity to circle off
		{
			float cyaw = atan2(cur[1], cur[0]) * 180.0f / M_PI;
			float e = vectoyaw(wish) - cyaw;
			int sgn;

			while (e > 180) { e -= 360; }
			while (e < -180) { e += 360; }
			sgn = (e >= 0) ? 1 : -1;
			VectorNormalize(cur);
			RotatePointAroundVector(wish, up, cur, build_angle * sgn);
			wish[2] = 0;
			VectorNormalize(wish);
		}
	}

	// Projection seam: wishdir -> fmove/smove through the view yaw (cancels).
	view_yaw = vectoyaw(wish);
	VectorSet(ang, 0, view_yaw, 0);
	trap_makevectors(ang);
	self->fb.desired_angle[PITCH] = 0;
	self->fb.desired_angle[YAW] = view_yaw;
	self->fb.desired_angle[ROLL] = 0;
	direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
	direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
	direction[2] = 0;
	*jumping = false;
	*firing = false;
	*impulse = 0;

	if (cvar("k_kbot_gj_traj"))
	{
		G_cprint("[gjapp] t=%.2f pos=%.0f,%.0f,%.0f along=%.0f lat=%.0f vh=%.0f "
				 "floor=%.0f og=%d\n",
				 now - gj_app_t0[slot], org[0], org[1], org[2], along_u, lat_n,
				 vh, floor, onground ? 1 : 0);
	}
	return true;
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
	// Continue an in-flight circle-jump run-up (experimental build path).
	if (gj_state[slot] == GJ_BUILD)
	{
		if (ISDEAD(self))
		{
			gj_state[slot] = GJ_IDLE;
			return false;
		}
		return GJ_BuildFrame(self, slot, gj_lane_active[slot], jumping, firing,
							 impulse, direction);
	}
	// Continue an in-flight E9 deliberate approach.
	if (gj_state[slot] == GJ_APPROACH)
	{
		if (ISDEAD(self))
		{
			gj_state[slot] = GJ_IDLE;
			return false;
		}
		return GJ_ApproachFrame(self, slot, gj_lane_active[slot], jumping, firing,
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

	// ---- E9 ACTIVE jump-intent (k_kbot_gj_active) ----
	// Deliberately set up the jump: if the nav goal is across a gap-lane and the
	// bot is on the takeoff side, drive to the lip / align / build, then launch.
	// Supersedes the passive incidental trigger when enabled.
	if (cvar("k_kbot_gj_active"))
	{
		int pick;

		// Re-engage cooldown: after a decline/abort we suppress re-engaging for a
		// short window so the bot is carried clear by nav and comes back with a
		// running start, instead of flickering engage/decline at the lip every frame.
		if (now < gj_app_supp[slot])
		{
			return false;
		}
		pick = GJ_PickIntentLane(self);

		if (pick >= 0)
		{
			gj_lane_active[slot] = pick;
			gj_app_t0[slot] = now;
			gj_build_sign[slot] = 0;
			gj_state[slot] = GJ_APPROACH;
			if (cvar("k_kbot_gj_gatelog"))
			{
				G_cprint("[gapjump] lane=%s result=APP_ENGAGE\n",
						 gj_lanes[pick].name);
			}
			return GJ_ApproachFrame(self, slot, pick, jumping, firing, impulse,
									direction);
		}
		return false; // no intent this frame -> vanilla nav
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

			// ---- E8.2 POSITION GATE ----
			// The passive trigger fires wherever nav wanders within the zone
			// (measured launches came from y=114..190, x=784..795 -- well off
			// the (455,146) lip). A launch from the wrong spot misses the
			// far-lip horn however well it is aimed. Require the bot at the near
			// lip: along-lane progress <= k_kbot_gj_maxprog (not past the lip)
			// and cross-lane offset <= k_kbot_gj_ymax (on the lane line).
			{
				vec3_t dir, rel;
				float lanelen, prog, perp;
				float maxprog = cvar("k_kbot_gj_maxprog");
				float ymax = cvar("k_kbot_gj_ymax");

				dir[0] = land[0] - take[0];
				dir[1] = land[1] - take[1];
				dir[2] = 0;
				lanelen = VectorLength(dir);
				if (lanelen > 1 && gj_state[slot] != GJ_CROSS)
				{
					VectorNormalize(dir);
					rel[0] = self->s.v.origin[0] - take[0];
					rel[1] = self->s.v.origin[1] - take[1];
					rel[2] = 0;
					prog = rel[0] * dir[0] + rel[1] * dir[1];
					perp = fabs(rel[0] * (-dir[1]) + rel[1] * dir[0]);
					if ((maxprog > 0 && prog > maxprog) ||
						(ymax > 0 && perp > ymax))
					{
						if (cvar("k_kbot_gj_gatelog"))
						{
							G_cprint("[gapjump] lane=%s result=DECLINE_POS "
									 "prog=%.0f perp=%.0f\n",
									 gj_lanes[pick].name, prog, perp);
						}
						gj_state[slot] = GJ_IDLE;
						return false;
					}
				}
			}

			// ---- E8 REQUIRED-SPEED GATE (the real in-match fix) ----
			// The dm3 gap is a level ~430u self-jump; the fixed +270 arc lands
			// short unless horizontal speed >= v_req (~600 launch here). E7 lost
			// -9.92 frags because the bot committed the jump at ~450 ups and fell
			// into the pit 126/128 times. Only commit when the approach speed can
			// actually clear the gap; otherwise DECLINE and let vanilla nav walk
			// the bot around (no pit suicide). k_kbot_gj_build (default off) may
			// try a circle-jump run-up to reach v_req before declining.
			{
				vec3_t hv;
				float vreq = GJ_RequiredSpeed(take, land);
				float gate = cvar("k_kbot_gj_gate");
				float vh;

				if (gate <= 0)
				{
					gate = 0.98f;
				}
				hv[0] = self->s.v.velocity[0];
				hv[1] = self->s.v.velocity[1];
				hv[2] = 0;
				vh = VectorLength(hv);

				if (gj_state[slot] != GJ_CROSS && vh < vreq * gate)
				{
					if (cvar("k_kbot_gj_build") > 0)
					{
						// Experimental: grounded circle-jump accel toward the
						// launch heading until fast enough, then release to CROSS.
						return GJ_BuildFrame(self, slot, pick, jumping, firing,
											 impulse, direction);
					}
					if (cvar("k_kbot_gj_gatelog"))
					{
						G_cprint("[gapjump] lane=%s result=DECLINE_SLOW vh=%.0f "
								 "vreq=%.0f pos=%.0f,%.0f,%.0f\n",
								 gj_lanes[pick].name, vh, vreq,
								 self->s.v.origin[0], self->s.v.origin[1],
								 self->s.v.origin[2]);
					}
					gj_state[slot] = GJ_IDLE;
					return false; // too slow -> decline, vanilla nav continues
				}
			}

			// ---- E8.2 ALIGNMENT GATE (the launch-heading fix) ----
			// In-match the bot arrives with its NAV heading, not the bow-around-
			// the-pillar launch heading (measured launch-err mean ~43 deg, up to
			// 96). The seeded trial lands ~100% only because it launches at err~0.
			// The ledge is far too small to turn a fast wrong-heading approach
			// onto the bow within the runway, so the safe, honest win is to GATE:
			// only commit when the velocity heading is already within
			// k_kbot_gj_align_tol of the required launch heading; otherwise
			// decline (vanilla nav walks on -- no doomed pit dive). Fewer jumps,
			// but the committed ones actually land.
			{
				float align_tol = cvar("k_kbot_gj_align_tol");

				if (align_tol > 0 && gj_state[slot] != GJ_CROSS)
				{
					float lb = GJ_Bearing(take, land) + GJ_LaneHeadOff(pick);
					float vyaw, aerr;
					vec3_t hv2;

					hv2[0] = self->s.v.velocity[0];
					hv2[1] = self->s.v.velocity[1];
					hv2[2] = 0;
					if (cvar("k_kbot_gj_head") > -360)
					{
						lb = cvar("k_kbot_gj_head");
					}
					vyaw = (VectorLength(hv2) > 1)
							   ? atan2(hv2[1], hv2[0]) * 180.0f / M_PI
							   : lb;
					aerr = lb - vyaw;
					while (aerr > 180) { aerr -= 360; }
					while (aerr < -180) { aerr += 360; }
					if (aerr < 0) { aerr = -aerr; }

					if (aerr > align_tol)
					{
						if (cvar("k_kbot_gj_gatelog"))
						{
							G_cprint("[gapjump] lane=%s result=DECLINE_YAW aerr=%.0f "
									 "tol=%.0f vyaw=%.0f want=%.0f\n",
									 gj_lanes[pick].name, aerr, align_tol, vyaw, lb);
						}
						gj_state[slot] = GJ_IDLE;
						return false; // arrival heading too far off -> decline
					}
				}
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
