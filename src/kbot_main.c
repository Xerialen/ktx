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
	if ((self->s.v.health + self->s.v.armorvalue) < KBot_WeakStack())
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
	// E12 (issue #11): optional mid-flight WAYPOINT for curve-required lanes.
	// {0,0,0} = none (straight lane, all pre-E12 behaviour byte-identical).
	// A wp lane launches AT the waypoint (seg1) and air-carves onto the landing
	// (seg2) once past it -- the 2-segment "curve-carve" both the RL descent and
	// the human reference line (blaze, mvd 224744 t~241.7) fly.
	vec3_t wp;
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
// E10b MIRROR LANE (RA-entrance <-> YA-high, owner "parallel exactly across the
// big ring"). E8.1 traced ONLY y=-146 (the pit's WIDEST point, all void) and
// wrongly concluded no southern lane exists. But the Ring and Quad platforms
// grow HORNS toward each other further south: fine floor-traces (this .so, cal
// mode) show the void narrows y=-200 (x~430..730, ~290u wide) -> y=-245
// (x~490..610) -> y=-290 (x~550..610). So there IS a second crossable file.
// Real human proof (mvd game 217186, .ParadokS t~533.05): crosses this southern
// void takeoff (444,-218,56) apex (566,-157,99) land (676,-268,56) ~232u level
// ~0.70s ~370 ups (== lane 0 difficulty), then chains south to YA.Quad
// (661,-588,116 = "YA high"). The human flies an S-curve (bow north over the
// wide void, then south to the quad horn); the bot's single carve-target can't
// track the S, and a straight line at y=-220..-290 clips the Ring/Quad HORNS
// (seeded: y-220/-245 0% FAIL_GAP, y-270 0% stuck-on-horn). But at y=-200 the
// void is clean and WIDE, but its SOLID ledges are recessed (ring x<=~440,
// quad x>=~715), so a lane must span ~290u lip-to-lip to seat + land on solid
// ground BOTH ways (a 250u span leaves the west landing in void -> ya2ra only
// 57% seeded). Lanes 2/3 therefore use the full clean span (430..720) at y=-200,
// straight, no pillar -> head_off 0. Seeded lands ~100% both ways. This is the
// same physical void the human uses (.ParadokS game 217186 crosses it y-218..-268
// at ~370 ups, then chains south to YA.Quad "YA-high"). NOTE: at 290u the launch
// floor (~479) exceeds the ~450 the bot reaches in-match, so in-match the RA/YA
// nav SEES + ROUTES + STAGES to this lane (gjroute prices quad, unreachable
// before) but mostly DECLINEs the launch -- the E9/E10 actuation wall, not a
// geometry defect. Seeded 100% is the landability proof.
// E12 RL DESCENT LANE (issue #11, lane 4). High-bridge deck (floor -48, origin
// -24) -> RL pad in the pent yard (floor -112, standing -88; RL item 1520,496).
// BSP-exact geometry (dm3.bsp faces, 07-04): the yard's south wall (y=368) has
// a 72u-tall FIRING SLOT, z -88..-16, x 1568..~1792, and the slot is a 16u-deep
// TUNNEL (wall band y368-384, sill top -88, ceiling -16). A jumping player
// (hull -24..+32) fits through ONLY with origin-z inside (-64,-48) at the wall
// -- a ~16u window on the descent arc, crossed t~0.75-0.80 after the hop.
// That pins the launch SPEED: path-to-slot ~392u from the deck => v0 ~490-519
// ("the needle"). blaze (mvd 224744 t~241.7) flies exactly this line at ~475+
// and clips the slot's UPPER lip by ~10u (origin -38 > -48) -- the human proof
// of both the line and the needle. The window-wall fin (x1440-1472, y>=112)
// forces the line to cross y112 east of x1488; air-turn radius at these speeds
// (~570u at 415) makes any compensating curve impossible, so the lane is
// STRAIGHT and the decisive lever is launch speed, not path shape. Landing on
// the slot SILL (org -64, fits under the -16 ceiling) or the pad behind both
// count -- table landing (1600,400) covers both within landrad. fail_z -150:
// below pad top, above the pool splash, so undershoot logs FAIL before the
// swim; waterlevel>=2 logs FAIL_WATER (E11c discharge guard). In-match the
// approach gate needs BOTH a floor and a CEILING (k_kbot_gj_rl_mul/_max) --
// too slow clips the sill, too fast clips the upper lip. Gated by k_kbot_gj_rl.
#define GJ_NUM_LANES 5
static const gj_lane_t gj_lanes[GJ_NUM_LANES] = {
	// 0: ring -> quad, northern lips, bow south (-30) around the pillar.
	{ "ring2quad", { 455, 146, 56 }, { 705, 146, 56 }, -40, -30, { 0, 0, 0 } },
	// 1: quad -> ring, reverse (mirror bow, +30).
	{ "quad2ring", { 705, 146, 56 }, { 455, 146, 56 }, -40, +30, { 0, 0, 0 } },
	// 2/3: RA<->YA southern parallel file -- full clean y=-200 span (solid both ends).
	//      Straight, no pillar -> head_off 0. Seeded ~100% both ways.
	{ "ra2ya", { 430, -200, 56 }, { 720, -200, 56 }, -40, 0, { 0, 0, 0 } },
	{ "ya2ra", { 720, -200, 56 }, { 430, -200, 56 }, -40, 0, { 0, 0, 0 } },
	// 4: bridge deck -> RL slot sill, straight line through the firing slot.
	//    ORIGIN-space design (the clip hull expands solids by the player half-
	//    extents): fin corner (1488,96), slot wall plane y352, slot jambs
	//    org-x 1584..1776, entry needle org-z (-64,-48). Bearing 67 deg clears
	//    the fin (y96 at x~1495) and crosses y352 at x~1603; the arc then sets
	//    feet down ON the sill (org -64) inside the slot. wp sits ON the line
	//    (x1520 -> y156); retune live via k_kbot_gj_rlwp without a rebuild.
	{ "bridge2rl", { 1454, 0, -24 }, { 1608, 368, -64 }, -150, 0, { 1520, 156, 0 } },
};

// Effective launch-heading offset for a lane: table default + cvar (for sweeps).
static float GJ_LaneHeadOff(int lane)
{
	float base = gj_lanes[lane].head_off;

	// E10c MIRROR-CARVE north launch bow (lanes 2/3 only, gated). Rotate the
	// launch heading toward NORTH so the hop leaves the lip aimed over the clean
	// void, clearing the central x=496 block, before the air-carve pulls it back
	// to the landing (mirror of Ring<->Quad's +-30 south bow around the pillar).
	// MIRRORED per direction: ra2ya flies EAST -> +north = CCW = +offset; ya2ra
	// flies WEST -> +north = CW = -offset. Lane-scoped so Ring<->Quad (0/1) and
	// the global k_kbot_gj_head_off sweep knob are unaffected.
	if (cvar("k_kbot_gj_mirrorcarve") && (lane == 2 || lane == 3))
	{
		base = (lane == 2) ? +40.0f : -45.0f;
	}
	return base + cvar("k_kbot_gj_head_off");
}

// E12: does this lane fly through a mid-flight waypoint? Returns true and fills
// wp. k_kbot_gj_rlwp "x y z" overrides the table value (retune without a
// rebuild, mirrors k_kbot_gj_to/_land). Lanes without a wp return false and are
// byte-identical to pre-E12 behaviour everywhere this is consulted.
static qbool GJ_LaneWaypoint(int lane, vec3_t wp)
{
	char buf[64];
	float x, y, z;

	if (gj_lanes[lane].wp[0] == 0 && gj_lanes[lane].wp[1] == 0)
	{
		return false;
	}
	VectorCopy(gj_lanes[lane].wp, wp);
	trap_cvar_string("k_kbot_gj_rlwp", buf, sizeof(buf));
	if (buf[0] && sscanf(buf, "%f %f %f", &x, &y, &z) == 3)
	{
		VectorSet(wp, x, y, z);
	}
	return true;
}

// E12: the point the LAUNCH aims at -- the waypoint on a wp lane (fly seg1),
// else the landing (straight lane, pre-E12 behaviour).
static void GJ_LaneLaunchTarget(int lane, vec3_t takeoff, vec3_t landing,
								vec3_t target)
{
	vec3_t wp;

	if (GJ_LaneWaypoint(lane, wp))
	{
		VectorCopy(wp, target);
		return;
	}
	VectorCopy(landing, target);
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
	float bow_north = 0; // E10c mirror lanes: +Y (NORTH) world-frame carve bow
	vec3_t mid, dir;
	float lanelen, prog, wpprog;

	// E10c MIRROR-CARVE north bow (lanes 2/3 only, gated). The human clears the
	// southern pinch by bowing NORTH over the clean void, then dropping onto the
	// quad horn. Applied lane-scoped here (NOT via the global k_kbot_gj_wp, which
	// would perturb Ring<->Quad lanes 0/1) so Ring<->Quad stays byte-identical.
	// Tunable in-match via k_kbot_gj_mcarve_bow WITHOUT touching lanes 0/1;
	// default 0 = fly the straight diagonal (already seeds ~100% both ways).
	if (cvar("k_kbot_gj_mirrorcarve") && (lane == 2 || lane == 3))
	{
		bow_north = cvar("k_kbot_gj_mcarve_bow");
		if (bow_north <= 0)
		{
			bow_north = 40; // baked default; set the cvar >0 to retune in-match
		}
	}

	// E12 wp-lane 2-segment carve: steer at the lane WAYPOINT until the bot's
	// progress along seg1 (takeoff->wp) reaches the waypoint minus a lead
	// distance (start the turn early -- the air-carve needs time to rotate the
	// velocity), then steer at the landing (seg2). Scoped to lanes that carry a
	// waypoint; all other lanes fall through to the pre-E12 logic untouched.
	{
		vec3_t lwp;

		if (GJ_LaneWaypoint(lane, lwp))
		{
			vec3_t u1;
			float seg1len, prog1, lead;

			u1[0] = lwp[0] - takeoff[0];
			u1[1] = lwp[1] - takeoff[1];
			u1[2] = 0;
			seg1len = VectorLength(u1);
			if (seg1len >= 1)
			{
				VectorNormalize(u1);
				prog1 = (org[0] - takeoff[0]) * u1[0] + (org[1] - takeoff[1]) * u1[1];
				lead = cvar("k_kbot_gj_wp_lead");
				if (lead <= 0)
				{
					lead = 48;
				}
				if (prog1 < seg1len - lead)
				{
					VectorCopy(lwp, target);
					return;
				}
			}
			VectorCopy(landing, target);
			return;
		}
	}

	if (wp == 0 && bow_north == 0)
	{
		VectorCopy(landing, target); // waypoint disabled
		return;
	}
	mid[0] = (takeoff[0] + landing[0]) * 0.5f;
	// wp bows SOUTH (lanes 0/1 around the pillar); bow_north bows NORTH (mirror
	// lanes 2/3 over the void). Only one is ever non-zero on a given lane.
	mid[1] = (takeoff[1] + landing[1]) * 0.5f - wp + bow_north;
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
static qbool gj_chain_on[MAX_CLIENTS];   // E12b chain-hop airborne (E1 carve live)
static int   gj_chain_flip[MAX_CLIENTS]; // E12b c=0 per-frame side alternator
static qbool gj_chain_flew[MAX_CLIENTS]; // E12b hop actually left the ground (the
										 // press+1 frame can still be grounded --
										 // without this latch the grounded clear
										 // killed the chain before flight)
static qbool gj_stage_on[MAX_CLIENTS];   // E10 stage-transition log latch
static float gj_route_log[MAX_CLIENTS];  // E10 [gjroute] log throttle

// Resolve the active lane geometry, honouring cvar overrides (retune without a
// rebuild). k_kbot_gj_to / _land are "x y z" strings; empty -> table value.
static void GJ_Geometry(int lane, vec3_t takeoff, vec3_t landing, float *fail_z)
{
	char buf[64];
	float x, y, z;

	VectorCopy(gj_lanes[lane].takeoff, takeoff);
	VectorCopy(gj_lanes[lane].landing, landing);
	*fail_z = gj_lanes[lane].fail_z;

	// E10c MIRROR-CARVE: relocate the mirror lanes 2/3 from the wide straight
	// y=-200 span (290u solid-to-solid) to the human's DIAGONAL lip-to-lip chord
	// (ParadokS, mvd 217186 t~533: takeoff (448,-214) -> land (677,-268) = 235u).
	// The shorter solid-to-solid chord drops the ballistic launch floor
	// (v_req * launch_mul 1.2) from ~479 to ~389 ups -- BELOW the ~410-417 the bot
	// actually builds in a 4on4 -- so the mirror jump CONVERTS in-match instead of
	// declining (APP_DECLINE_SLOW). Both lips are solid (the human takes off/lands
	// there both ways), so seeded still lands ~100% both directions. Gated by
	// k_kbot_gj_mirrorcarve (default 0 -> lanes 2/3 keep the shipped 290u straight
	// geometry byte-for-byte). Ring<->Quad lanes 0/1 are NEVER touched here.
	if (cvar("k_kbot_gj_mirrorcarve") && (lane == 2 || lane == 3))
	{
		// Both directions share the RING lip (448,-214) and use their OWN quad
		// lip. QUAD-SIDE ASYMMETRY (traced): the quad plate can be LANDED at the
		// deep-south spot (677,-268) but launching WEST from there is blocked by a
		// solid rim wall at x~656; the clean WEST-launch lip sits ~53u further
		// NORTH at (677,-215). So ra2ya lands south, ya2ra takes off north -- each
		// picks the lip that works for its travel direction (both ~230u chord,
		// launch floor ~379-389 < the ~415 the bot builds -> converts in-match).
		if (lane == 2)   // ra2ya: ring -> quad (land the deep-south quad spot)
		{
			VectorSet(takeoff, 448, -214, 56);
			VectorSet(landing, 677, -268, 56);
		}
		else             // ya2ra: quad -> ring (launch the clean north quad lip)
		{
			VectorSet(takeoff, 677, -215, 56);
			VectorSet(landing, 448, -214, 56);
		}
	}

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

// E12: lane-aware horizontal distance -- the POLYLINE length through the lane
// waypoint when one exists, else the straight chord. Keeps the ballistic
// required-speed honest on curve lanes (the arc is flown, not the chord).
static float GJ_LaneDistance(int lane, vec3_t takeoff, vec3_t landing)
{
	vec3_t wp;
	float dx, dy, d1, d2;

	if (GJ_LaneWaypoint(lane, wp))
	{
		dx = wp[0] - takeoff[0];
		dy = wp[1] - takeoff[1];
		d1 = sqrt(dx * dx + dy * dy);
		dx = landing[0] - wp[0];
		dy = landing[1] - wp[1];
		d2 = sqrt(dx * dx + dy * dy);
		return d1 + d2;
	}
	dx = landing[0] - takeoff[0];
	dy = landing[1] - takeoff[1];
	return sqrt(dx * dx + dy * dy);
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

// E12: lane-aware required speed -- polyline distance on wp lanes, straight
// chord otherwise (identical to GJ_RequiredSpeed for lanes 0..3).
static float GJ_LaneRequiredSpeed(int lane, vec3_t takeoff, vec3_t landing)
{
	float over = cvar("k_kbot_gj_vreq");
	float dz = landing[2] - takeoff[2];
	float D = GJ_LaneDistance(lane, takeoff, landing);
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
	// lands laterally on dm3). E12: wp lanes aim seg1 (at the waypoint).
	// k_kbot_gj_head is an absolute override for sweeps.
	{
		vec3_t ltgt;

		GJ_LaneLaunchTarget(lane, takeoff, landing, ltgt);
		bearing = GJ_Bearing(takeoff, ltgt) + GJ_LaneHeadOff(lane);
	}
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

	// E12 AIR-SEAT (lane 4, k_kbot_gj_airseat, default on): seed the bot
	// ALREADY AIRBORNE with the hop's vz=+270 and the exact v0. The grounded
	// seat costs a variable 3-5 friction frames before the engine actuates the
	// hop (measured: v0 504 decayed to launch ~407, jitter ~1 frame = ~26 ups)
	// -- fatal for the slot's ~29-ups launch window. Injecting the launched
	// state makes the seeded arc deterministic; the in-match path still uses
	// the real approach + hop.
	if (lane == 4 && cvar("k_kbot_gj_airseat") >= 0)
	{
		org[2] += 1;
		setorigin(self, PASSVEC3(org));
		self->s.v.flags = (int)self->s.v.flags & ~FL_ONGROUND;
		VectorScale(fwd, v0, self->s.v.velocity);
		self->s.v.velocity[2] = 270;
		return;
	}
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

	// Fixed launch heading (ballistic aim at the far ledge + per-lane offset,
	// E12: wp lanes aim seg1); used on the ground/launch frame so the hop
	// leaves at the lane heading.
	{
		vec3_t ltgt;

		GJ_LaneLaunchTarget(lane, takeoff, landing, ltgt);
		launch_bearing = GJ_Bearing(takeoff, ltgt) + GJ_LaneHeadOff(lane);
	}
	if (cvar("k_kbot_gj_head") > -360)
	{
		launch_bearing = cvar("k_kbot_gj_head");
	}
	vreq = GJ_LaneRequiredSpeed(lane, takeoff, landing);

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

	// E10c LAUNCH-AIM (mirror lanes only, gated). On the committed launch frame,
	// aim the horizontal launch velocity along the lane's bowed launch heading
	// (magnitude preserved). WHY: in-match the bot arrives heading along the
	// corridor axis u (straight at the far lip); to clear the SE block at x~496 it
	// would have to air-turn ~40 deg, which scrubs ~250 ups and drops it into the
	// void (measured: in-match FAIL_GAP at x~600, z~-42). The seeded trial lands
	// ~100% ONLY because GJ_Seat injects this pre-bowed launch velocity. Aiming
	// the ALREADY-COMMITTED jump's launch here delivers the same pre-bowed launch
	// in a real match, at full speed. Strictly scoped to a committed, enemy-free
	// gap-jump on lanes 2/3 (the combat-yield in KBot_GapjumpFrame/GJ_ApproachFrame
	// bails on enemy_visible BEFORE any launch), so it never touches combat
	// movement. k_kbot_gj_aimlaunch (default on) can disable it for A/B isolation.
	if (press && speed > 1 && cvar("k_kbot_gj_aimlaunch") >= 0 &&
		((cvar("k_kbot_gj_mirrorcarve") && (lane == 2 || lane == 3)) ||
		 (lane == 4 && cvar("k_kbot_gj_rl"))))
	{
		vec3_t la;
		la[0] = 0;
		la[1] = launch_bearing;
		la[2] = 0;

		trap_makevectors(la);
		self->s.v.velocity[0] = g_globalvars.v_forward[0] * speed;
		self->s.v.velocity[1] = g_globalvars.v_forward[1] * speed;
	}

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
		G_cprint("[gapjump] lane=%s slot=%d name=%s trial=%d result=LAND land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f vreq=%.0f\n",
				 gj_lanes[lane].name, slot, self->netname, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot], vreq);
		KDLog_Play(self, gj_lanes[lane].name, "land", NULL); // KDLOG
		gj_state[slot] = GJ_COOL;
		gj_cool_t0[slot] = now;
		return true;
	}
	// E12 WATER-OVERSHOOT GUARD (lane 4): the RL pad is flanked by the pent-yard
	// pool; any water entry is a hard FAIL (E11c discharge rule -- a stacked bot
	// must never dive). waterlevel >= 2 = origin submerged = swim physics took
	// over. Logged separately from FAIL_GAP so the A/B can count water entries.
	if (lane == 4 && ((int)self->s.v.waterlevel >= 2))
	{
		G_cprint("[gapjump] lane=%s trial=%d result=FAIL_WATER land_pos=%.0f,%.0f,%.0f "
				 "hdist=%.0f peak_speed=%.0f tair=%.2f vreq=%.0f\n",
				 gj_lanes[lane].name, gj_trial[slot], org[0], org[1], org[2],
				 hdist, gj_peak[slot], now - gj_t0[slot], vreq);
		KDLog_Play(self, gj_lanes[lane].name, "fail", "water"); // KDLOG
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
		KDLog_Play(self, gj_lanes[lane].name, "fail", "gap"); // KDLOG
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
		KDLog_Play(self, gj_lanes[lane].name, "fail", "timeout"); // KDLOG
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
	vreq = GJ_LaneRequiredSpeed(lane, takeoff, landing);
	gate = cvar("k_kbot_gj_gate");
	if (gate <= 0)
	{
		gate = 0.98f;
	}
	{
		vec3_t ltgt;

		GJ_LaneLaunchTarget(lane, takeoff, landing, ltgt);
		launch_bearing = GJ_Bearing(takeoff, ltgt) + GJ_LaneHeadOff(lane);
	}
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
		KDLog_Play(self, gj_lanes[lane].name, "abort", "build"); // KDLOG
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
// (behind the lip along the lane, within the given corridor band, on the ledge)
// AND the GOAL is across the gap (projects past the lane midpoint toward
// landing). Returns lane index, or -1. Nearest-to-lip wins. Core shared by the
// E9 intent box (tight bounds) and the E10 route/staging region (wide bounds).
static int GJ_PickLaneWithin(gedict_t *self, float back, float pmax, float zband)
{
	int i, pick = -1;
	float best = 1e30f;
	vec3_t org, goalorg;
	gedict_t *goal;

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
		float lanelen, along, lat, galong, fz;

		if ((i == 4) && !cvar("k_kbot_gj_rl"))
		{
			continue; // E12 RL lane disabled -> invisible to intent/route
		}
		// Use GJ_Geometry (not raw table) so the E10c mirror-carve relocation of
		// lanes 2/3 is honoured by the active-path intent/route lane picker too.
		GJ_Geometry(i, take, land, &fz);
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

// E9 intent box: the tight engage region right behind the lip.
static int GJ_PickIntentLane(gedict_t *self)
{
	float back = cvar("k_kbot_gj_intent_back");
	float pmax = cvar("k_kbot_gj_intent_perp");
	float zband = cvar("k_kbot_gj_intent_zband");

	if (back <= 0)  { back = 384; }
	if (pmax <= 0)  { pmax = 176; }
	if (zband <= 0) { zband = 56; }

	return GJ_PickLaneWithin(self, back, pmax, zband);
}

// E10 route/staging region: the whole takeoff-side plate. A hit means the
// gap-jump is a plausible route for the current goal, not yet a committed run.
static int GJ_PickRouteLane(gedict_t *self)
{
	float back = cvar("k_kbot_gj_route_back");
	float pmax = cvar("k_kbot_gj_route_lat");
	float zband = cvar("k_kbot_gj_intent_zband");

	if (back <= 0)  { back = 512; }
	if (pmax <= 0)  { pmax = 352; }
	if (zband <= 0) { zband = 56; }

	return GJ_PickLaneWithin(self, back, pmax, zband);
}

// E10 ROUTE SHIM (called from EvalGoal for kbots when k_kbot_gj_route != 0).
// The shared marker graph has no ring<->quad hop edge, so the vanilla travel
// table prices that trip at the walk-around time (~8-10 s). Price the jump as
// a route instead: walk-to-lip + edge_time + walk-from-landing (straight-line
// legs -- both plates are open floor). Returning the cheaper time changes this
// bot's GOAL SELECTION only; the shared subzone tables (and thus baseline
// frogbots) are untouched. goal_time here is pure travel time -- EvalGoal
// max()es the respawn wait in afterwards, so item timing math is preserved.
float KBot_GJ_RouteShim(gedict_t *self, gedict_t *goal_entity, float goal_time)
{
	int i, slot = NUM_FOR_EDICT(self) - 1;
	float sv_ms, edge_time, back, latmax, zband;
	vec3_t org, goalorg;

	if (!cvar("k_kbot_gapjump") || !cvar("k_kbot_gj_active"))
	{
		return goal_time;
	}
	if ((slot < 0) || (slot >= MAX_CLIENTS) || !goal_entity || (goal_entity == world))
	{
		return goal_time;
	}

	sv_ms = cvar("sv_maxspeed");
	if (sv_ms <= 0) { sv_ms = 320; }
	edge_time = cvar("k_kbot_gj_edge_time");
	if (edge_time <= 0) { edge_time = 1.3f; }
	back = cvar("k_kbot_gj_route_back");
	if (back <= 0) { back = 512; }
	latmax = cvar("k_kbot_gj_route_lat");
	if (latmax <= 0) { latmax = 352; }
	zband = cvar("k_kbot_gj_intent_zband");
	if (zband <= 0) { zband = 56; }

	VectorCopy(self->s.v.origin, org);
	VectorCopy(goal_entity->s.v.origin, goalorg);

	for (i = 0; i < GJ_NUM_LANES; i++)
	{
		vec3_t take, land, u;
		float fail_z, lanelen, along, lat, galong, d_in, d_out, t_gj;

		if ((i == 4) && !cvar("k_kbot_gj_rl"))
		{
			continue; // E12 RL lane disabled -> not priced as a route edge
		}
		GJ_Geometry(i, take, land, &fail_z);
		u[0] = land[0] - take[0];
		u[1] = land[1] - take[1];
		u[2] = 0;
		lanelen = VectorLength(u);
		if (lanelen < 1)
		{
			continue;
		}
		VectorNormalize(u);
		along = (org[0] - take[0]) * u[0] + (org[1] - take[1]) * u[1];
		lat = (org[0] - take[0]) * (-u[1]) + (org[1] - take[1]) * u[0];
		if ((along > 32) || (along < -back))
		{
			continue;
		}
		if (fabs(lat) > latmax)
		{
			continue;
		}
		if (fabs(org[2] - take[2]) > zband)
		{
			continue;
		}
		galong = (goalorg[0] - take[0]) * u[0] + (goalorg[1] - take[1]) * u[1];
		if (galong < lanelen * 0.5f)
		{
			continue;
		}
		d_in = sqrt((take[0] - org[0]) * (take[0] - org[0])
					+ (take[1] - org[1]) * (take[1] - org[1]));
		d_out = sqrt((goalorg[0] - land[0]) * (goalorg[0] - land[0])
					 + (goalorg[1] - land[1]) * (goalorg[1] - land[1]));
		t_gj = (d_in / sv_ms) + edge_time + (d_out / sv_ms);
		if (t_gj < goal_time)
		{
			if (cvar("k_kbot_gj_gatelog")
					&& (((g_globalvars.time - gj_route_log[slot]) >= 1.0f)
						|| (gj_route_log[slot] > g_globalvars.time)))
			{
				gj_route_log[slot] = g_globalvars.time;
				G_cprint("[gjroute] lane=%s slot=%d goal=%s t_gj=%.2f t_std=%.2f\n",
						 gj_lanes[i].name, slot, goal_entity->classname, t_gj, goal_time);
			}
			return t_gj;
		}
	}

	return goal_time;
}

// E12b owner rule: only attempt the RL jump while UNSEEN. If any live enemy
// has line-of-sight to the bot, do not engage or continue the setup -- the
// build orbit and the committed hop are combat-defenseless, so being watched
// means being shot mid-jump. Plain LOS regardless of enemy facing (an enemy
// looking away can turn faster than the 4-8 s setup completes). Two sample
// points (feet-origin + eye height) match the engine's VisibleEntity idiom.
static qbool GJ_SeenByEnemy(gedict_t *self)
{
	gedict_t *p;
	vec3_t eye, tgt;

	for (p = world; (p = find_plr(p)); )
	{
		if ((p == self) || ISDEAD(p))
		{
			continue;
		}
		if (getteam(self)[0] && streq(getteam(p), getteam(self)))
		{
			continue; // teammate
		}
		eye[0] = p->s.v.origin[0];
		eye[1] = p->s.v.origin[1];
		eye[2] = p->s.v.origin[2] + 22;
		tgt[0] = self->s.v.origin[0];
		tgt[1] = self->s.v.origin[1];
		tgt[2] = self->s.v.origin[2];
		traceline(PASSVEC3(eye), PASSVEC3(tgt), true, p);
		if (g_globalvars.trace_fraction == 1)
		{
			return true;
		}
		tgt[2] = self->s.v.origin[2] + 22;
		traceline(PASSVEC3(eye), PASSVEC3(tgt), true, p);
		if (g_globalvars.trace_fraction == 1)
		{
			return true;
		}
	}

	return false;
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
	vreq = GJ_LaneRequiredSpeed(lane, take, land);
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
	// E12 RL lane: no central pillar, and the lane axis has a large +Y component
	// so raw world-Y offset mixes along and lateral. Use the TRUE perpendicular
	// (the asymmetric north gate below then acts as a plain +- band).
	if (lane == 4)
	{
		lat_n = rel[0] * (-u[1]) + rel[1] * u[0];
	}

	{
		vec3_t hv;
		hv[0] = self->s.v.velocity[0];
		hv[1] = self->s.v.velocity[1];
		hv[2] = 0;

		vh = VectorLength(hv);
	}
	apptime = cvar("k_kbot_gj_apptime");
	if (apptime <= 0) { apptime = 3.5f; }
	// E12 RL lane: the slot window sits ~60 ups above what a 42-deg ground
	// circle can build (wishspeed/cos(42) caps ~430), so give the build more
	// time on the long deck runway; the steeper lane build angle (below)
	// raises the cap itself.
	if (lane == 4)
	{
		// 8s with the chain (the build orbit needs a few loops to carry 430);
		// E12's 6s otherwise. Timeout is a safe decline either way.
		apptime = cvar("k_kbot_gj_chain") ? 8.0f : 6.0f;
	}
	launch_win = cvar("k_kbot_gj_launch_win");
	if (launch_win <= 0) { launch_win = 48; }
	// E12 RL lane: a +-48u along-window shifts the path-to-slot by +-48u of
	// arc height timing at the wall. The slot needle tolerates ~+-24 (the
	// wall face above catches too-high arrivals and drops them onto the sill;
	// the 18u air step-up catches slightly-low ones) but not the full 48.
	if (lane == 4)
	{
		float rw = cvar("k_kbot_gj_rl_win");

		launch_win = (rw > 0) ? rw : 24;
	}
	launch_perp = cvar("k_kbot_gj_launch_perp");
	if (launch_perp <= 0) { launch_perp = 28; }
	// E12b: the firing slot is 192u wide in x (1584-1776) -- the needle is in
	// z, not lateral. A wider lateral band multiplies chain-trigger
	// coincidences (the orbit exit heading is the scarce resource) at no slot
	// cost: +-40 shifts the wall crossing well inside the slot span.
	if ((lane == 4) && cvar("k_kbot_gj_chain")) { launch_perp = 40; }
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
		// E10c: the mirror lanes fly a NORTH-bowed arc (clearing the x~496 SE
		// block), whose carve costs more speed than the straight Ring<->Quad hop,
		// so a launch AT vreq*1.2 still lands short in the void (measured in-match:
		// lands need peak ~425+, floor-1.2 lets ~412 launches commit -> pit). Raise
		// the mirror-lane floor so only launches fast enough to actually clear the
		// bowed path commit; slower approaches DECLINE (safe) instead of diving.
		// Lane-scoped (Ring<->Quad 0/1 keep 1.2). Tunable via k_kbot_gj_mcarve_mul.
		if (cvar("k_kbot_gj_mirrorcarve") && (lane == 2 || lane == 3))
		{
			float mm = cvar("k_kbot_gj_mcarve_mul");

			// 1.20 = the A/B-validated converting value (mirror lanes launch +
			// land in-match). Raising it cuts pit-falls but collapses launch
			// frequency (the ~415-built vs ~425-needed wall) -- see findings.
			mul = (mm > 0) ? mm : 1.20f;
		}
		// E12 RL lane: the slot needle needs a SPEED WINDOW, not just a floor
		// (too slow clips the sill, too fast the upper lip). Floor via rl_mul:
		// 0.99*vreq ~ 459 = just above the measured hard edge (455 seeded 96%,
		// 440 0%) -- the 18u air step-up catches low-edge entries.
		if (lane == 4)
		{
			float rm = cvar("k_kbot_gj_rl_mul");

			mul = (rm > 0) ? rm : 0.99f;
		}
		floor = vreq * mul;
	}
	fast = vh >= floor;
	// E12 RL lane speed CEILING: launching above it overshoots the slot's upper
	// lip (org > -48 at the wall) -> upper wall face -> pool. Decline instead.
	if (lane == 4)
	{
		float rmax = cvar("k_kbot_gj_rl_max");
		float ceilv = vreq * ((rmax > 0) ? rmax : 1.13f);

		if (vh > ceilv)
		{
			fast = false; // treated as not-launchable this frame
		}
	}

	// Velocity heading error vs the corridor axis (the direction we deliver).
	vyaw = (vh > 1) ? atan2(self->s.v.velocity[1], self->s.v.velocity[0])
						  * 180.0f / M_PI
					: u_yaw;
	aerr = u_yaw - vyaw;
	while (aerr > 180) { aerr -= 360; }
	while (aerr < -180) { aerr += 360; }
	if (aerr < 0) { aerr = -aerr; }
	aligned = (aerr <= align_tol);

	// Abort -> vanilla nav (never launch a doomed jump). EXCEPTION: mid-air in
	// the chain hop -- the flight is 0.675 s bounded and the touchdown frame
	// re-runs every grounded gate (incl. this timeout), so let it finish
	// instead of discarding a completed build on a boundary technicality.
	if (((now - gj_app_t0[slot]) > apptime) &&
		!((lane == 4) && gj_chain_on[slot] && !onground))
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_ABORT_TIMEOUT along=%.0f lat=%.0f "
					 "vh=%.0f floor=%.0f\n",
					 gj_lanes[lane].name, along_u, lat_n, vh, floor);
		}
		KDLog_Play(self, gj_lanes[lane].name, "abort", "app_timeout"); // KDLOG
			gj_state[slot] = GJ_IDLE;
		return false;
	}
	// Enemy showed up mid-setup -> yield. EXCEPTION: mid-air in the chain hop
	// (0.675 s of committed ballistics, same rationale as CROSS) -- yielding
	// there just discards the setup without helping combat. The touchdown
	// frame is grounded, so BOTH this yield and the unseen rule below still
	// gate the actual launch.
	if (self->fb.enemy_visible &&
		!((lane == 4) && gj_chain_on[slot] && !onground))
	{
		gj_state[slot] = GJ_IDLE;
		return false;
	}
	// Owner rule (2026-07-05): abort the RL setup the moment ANY enemy can see
	// the bot -- not just when the bot sees an enemy. Grounded phases only;
	// a committed hop/flight resolves via the normal CROSS machinery.
	if ((lane == 4) && onground && cvar("k_kbot_gj_rl_unseen") &&
		GJ_SeenByEnemy(self))
	{
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gapjump] lane=%s result=APP_YIELD_SEEN along=%.0f vh=%.0f\n",
					 gj_lanes[lane].name, along_u, vh);
		}
		KDLog_Play(self, gj_lanes[lane].name, "yield", "seen"); // KDLOG
			gj_state[slot] = GJ_IDLE;
		return false;
	}

	// E12b chain-hop FLIGHT: the E1 c=0 carve law (lab-validated: gain constant
	// K=67593 ups^2/s, 0.14% off theory) applied for exactly one hop. Below the
	// hold target the wishdir is EXACTLY perpendicular to velocity with per-frame
	// side alternation (max air-accel gain, zero net rotation = straight line);
	// at/above target, wish rides along velocity (hold, no gain, no scrub). The
	// view follows the velocity yaw (E1 rig idiom: view pinned, smove does the
	// work). The touchdown frame is grounded, so on the next call it falls
	// through to the launch gate below (launch-aim snap + GJ_Cross), which is
	// the proven in-match launch path. Timeout/enemy yields above still apply
	// mid-flight (bot is over the deck the whole hop -- bailing is safe).
	if ((lane == 4) && gj_chain_on[slot] && !onground && cvar("k_kbot_gj_chain"))
	{
		float rm = cvar("k_kbot_gj_rl_mul");
		float rx = cvar("k_kbot_gj_rl_max");
		float tgt_v = vreq * 0.5f * (((rm > 0) ? rm : 0.99f)
									 + ((rx > 0) ? rx : 1.09f));
		vec3_t cur, cwish, cang;
		float cyaw;

		gj_chain_flew[slot] = true;
		cur[0] = self->s.v.velocity[0];
		cur[1] = self->s.v.velocity[1];
		cur[2] = 0;
		if (VectorNormalize(cur) <= 0)
		{
			cur[0] = u[0];
			cur[1] = u[1];
			cur[2] = 0;
		}
		if (vh < tgt_v)
		{
			vec3_t up = { 0, 0, 1 };
			float side = gj_chain_flip[slot] ? 1.0f : -1.0f;

			gj_chain_flip[slot] = !gj_chain_flip[slot];
			RotatePointAroundVector(cwish, up, cur, 90.0f * side);
			cwish[2] = 0;
			VectorNormalize(cwish);
		}
		else
		{
			VectorCopy(cur, cwish);
		}
		cyaw = vectoyaw(cur);
		VectorSet(cang, 0, cyaw, 0);
		trap_makevectors(cang);
		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = cyaw;
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = DotProduct(g_globalvars.v_forward, cwish) * 800;
		direction[1] = DotProduct(g_globalvars.v_right, cwish) * 800;
		direction[2] = 0;
		*jumping = false;
		*firing = false;
		*impulse = 0;
		return true;
	}
	if (onground && gj_chain_on[slot] && gj_chain_flew[slot])
	{
		gj_chain_on[slot] = false; // touchdown: hand back to the grounded gates
		gj_chain_flew[slot] = false;
		if (cvar("k_kbot_gj_gatelog"))
		{
			G_cprint("[gjchain] lane=%s DOWN vh=%.0f along=%.0f lat=%.0f floor=%.0f\n",
					 gj_lanes[lane].name, vh, along_u, lat_n, floor);
		}
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
		if (lane == 4) { north_max = launch_perp; } // no pillar: symmetric band
		// E12b: WEST-side launches (positive lat) die in flight -- the cross
		// air-carve's eastward correction scrubs the speed the slot needs
		// (b3/b4: all 3 lat>+8 launches peaked ~482 and fell short at y~285;
		// all 5 lat<=+5 launches LANDed, peaks 519-531). Asymmetric band:
		// open toward the east (negative lat), capped +5 on the west.
		if ((lane == 4) && cvar("k_kbot_gj_chain")) { north_max = 5; }
		at_lip = (along_u >= -launch_win && along_u <= launch_win) &&
				 (lat_n >= -launch_perp && lat_n <= north_max) && aligned;
	}
	// E12b: for chain-mode lane-4 launches the REAL slot constraint is the
	// coupled (distance, speed) wall-crossing height, not the independent
	// along/speed windows (b1 FAIL_GAP: along -23 at 464 crossed at z -92).
	// Require the predicted crossing z from HERE at CURRENT speed to sit in
	// the slot entry band; low/fast couplings the static windows would have
	// wrongly accepted are rejected, and vice versa.
	if ((lane == 4) && cvar("k_kbot_gj_chain") && at_lip && fast)
	{
		// The flight leaves along the LAUNCH-AIM direction (velocity snapped
		// at the waypoint from HERE), so the wall distance must use the aim
		// direction's y-fraction -- NOT the lane axis. b3 FAIL: a launch at
		// lat +24 (west) aims 0.862-y at the wp vs the axis 0.922 -> 26u more
		// wall distance -> crossing -86, not the axis-model -61.
		vec3_t wpv, ad;
		float ady, dwall, tw, zpredl;

		if (!GJ_LaneWaypoint(lane, wpv))
		{
			VectorCopy(land, wpv);
		}
		ad[0] = wpv[0] - org[0];
		ad[1] = wpv[1] - org[1];
		ad[2] = 0;
		VectorNormalize(ad);
		ady = (ad[1] > 0.30f) ? ad[1] : 0.92f;
		dwall = (352.0f - org[1]) / ady;
		tw = (vh > 1) ? (dwall / vh) : 9.9f;
		zpredl = org[2] + 270.0f * tw - 400.0f * tw * tw;

		// [-75,-36]: realized crossings run ~7u LOWER than this model (b3:
		// predicted -77 fell just under the -82 step-up limit; the two LANDs
		// modeled -69 and crossed ~-76). So the model window [-75,-36] IS the
		// physical catcher span [-82,-43] after bias.
		if ((zpredl < -75.0f) || (zpredl > -36.0f))
		{
			fast = false; // wrong coupling this frame -> not launchable
		}
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
		KDLog_Play(self, gj_lanes[lane].name, "launch", NULL); // KDLOG
		gj_t0[slot] = now;
		gj_peak[slot] = 0;
		gj_flip[slot] = 1;
		gj_jump_latch[slot] = false;
		gj_has_flown[slot] = false;
		gj_state[slot] = GJ_CROSS;
		return GJ_Cross(self, slot, lane, jumping, firing, impulse, direction);
	}

	// E12b: while the chain build-orbit is maneuvering (lane 4, below exit
	// speed), the 180-degree turnaround arc legitimately sweeps along +10..+35
	// north of the take (probe: DECLINE_PAST/SLOW fired mid-turn at aerr
	// 93-177 and killed every orbit). Suppress both grounded declines inside
	// the maneuver band; keep a HARD past-limit (along > 64) as the edge/fin
	// safety -- beyond that the bot really is drifting off the deck plate.
	{
		qbool orbiting = (lane == 4) && cvar("k_kbot_gj_chain") &&
						 (vh < ((cvar("k_kbot_gj_chain_exit") > 0)
								? cvar("k_kbot_gj_chain_exit") : 430));

		if (orbiting && onground && (along_u > 64))
		{
			if (cvar("k_kbot_gj_gatelog"))
			{
				G_cprint("[gapjump] lane=%s result=APP_DECLINE_PAST along=%.0f "
						 "lat=%.0f vh=%.0f aerr=%.0f\n",
						 gj_lanes[lane].name, along_u, lat_n, vh, aerr);
			}
			KDLog_Play(self, gj_lanes[lane].name, "decline", "past_orbit"); // KDLOG
			gj_state[slot] = GJ_IDLE;
			return false;
		}
		if (orbiting)
		{
			goto gj_app_drive; // skip the lip declines during the maneuver
		}
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
		KDLog_Play(self, gj_lanes[lane].name, "decline", "past"); // KDLOG
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
		KDLog_Play(self, gj_lanes[lane].name, "decline", "slow"); // KDLOG
			gj_state[slot] = GJ_IDLE;
		return false;
	}

gj_app_drive:
	// E12b CHAIN-HOP TRIGGER (D5-revisit, scoped): the ground circle-build caps
	// at wishspeed/cos(angle) ~450, below the slot floor (~459) -- the E12
	// actuation wall. ONE airborne E1-carve hop bridges it: a flat hop lasts
	// T = 2*270/800 = 0.675 s and the E1 law gains K*T ~= 45600 in v^2
	// (~430 -> ~480, mid slot-window). Fire on the grounded frame whose
	// predicted touchdown -- one hop long, along the CURRENT velocity -- lands
	// inside the launch box with predicted speed inside the slot speed window;
	// the flight branch above then carves, and the touchdown frame takes the
	// normal launch gate. Both miss modes are SAFE on this lane: short -> the
	// grounded gates re-take over (decline/rebuild); long -> the deck continues
	// ~100u past the lip along u (BSP: floor to y=112/160) -> DECLINE_PAST.
	// Same-frame hop on the press frame means ground friction never applies
	// (the E3 motor's documented contact-frame idiom).
	if ((lane == 4) && onground && cvar("k_kbot_gj_chain") &&
		(along_u < -launch_win) && (vh > 1))
	{
		float cmin = cvar("k_kbot_gj_chain_min");
		float rm = cvar("k_kbot_gj_rl_mul");
		float rx = cvar("k_kbot_gj_rl_max");
		float tgt_v = vreq * 0.5f * (((rm > 0) ? rm : 0.99f)
									 + ((rx > 0) ? rx : 1.09f));
		float vpred, lhop, pa, pl;
		vec3_t land, lrel;

		if (cmin <= 0) { cmin = 410; }
		vpred = sqrt(vh * vh + 67593.0f * 0.675f);
		if (vpred > tgt_v) { vpred = tgt_v; }
		if ((vh >= cmin) && (vpred >= floor))
		{
			lhop = 0.5f * (vh + vpred) * 0.675f;
			land[0] = org[0] + (self->s.v.velocity[0] / vh) * lhop;
			land[1] = org[1] + (self->s.v.velocity[1] / vh) * lhop;
			land[2] = 0;
			lrel[0] = land[0] - take[0];
			lrel[1] = land[1] - take[1];
			lrel[2] = 0;
			pa = DotProduct(lrel, u);
			pl = lrel[0] * (-u[1]) + lrel[1] * u[0];
			{
				float ctol = cvar("k_kbot_gj_chain_tol");
				float dwall, tw, zpred;

				if (ctol <= 0) { ctol = 20; }
				// SLOT-CROSSING PREDICTION: the slot tolerance is a COUPLED
				// (distance, speed) window, not independent along/speed bands
				// (b1 FAIL: launch along -23 at 464 -> wall z -92, under the
				// sill; probe-4 LAND: along +16 at 470 -> z -55). Gate the hop
				// on the predicted wall-crossing height of the LAUNCH that the
				// touchdown would produce: z(t) = -24 + 270 t - 400 t^2 at
				// t = D/v, D = distance from touchdown to the wall plane
				// (origin y 352) along the flight. Window [-75,-48]: sill
				// entry to slot mid, leaving the step-up as low-side margin.
				dwall = (352.0f - land[1]) / ((u[1] > 0.1f) ? u[1] : 0.92f);
				tw = dwall / vpred;
				zpred = -24.0f + 270.0f * tw - 400.0f * tw * tw;
				// [-72,-45]: aim the MIDDLE of the launch gate's [-82,-40].
				// Realized crossings ran ~7u LOWER than predicted (touchdown
				// lands ~6u short of pred -- the press+1 actuation frame), so
				// a mid-biased prediction keeps the realized coupling inside
				// the gate with margin on both sides.
				// pl in [-24,+12], vs the launch gate's west cap +5: realized
				// touchdowns drift ~8u EAST of the prediction (b4: pl +16 ->
				// lat +8, +23 -> +12, +7 -> -4), so a +12 prediction lands
				// ~+4 and passes the gate; capping the PREDICTION at +5
				// choked trigger frequency 3x (b5: 2 hops vs b4: 6) without
				// adding safety -- the gate sees the REAL lat either way.
				if ((pa >= -ctol) && (pa <= launch_win) &&
					(pl >= -24.0f) && (pl <= 12.0f) &&
					(zpred >= -72.0f) && (zpred <= -45.0f))
				{
					if (!gj_chain_on[slot] && cvar("k_kbot_gj_gatelog"))
					{
						G_cprint("[gjchain] lane=%s HOP vh=%.0f vpred=%.0f "
								 "lhop=%.0f pa=%.0f pl=%.0f along=%.0f\n",
								 gj_lanes[lane].name, vh, vpred, lhop, pa, pl,
								 along_u);
					}
					if (!gj_chain_on[slot])
					{
						KDLog_Play(self, gj_lanes[lane].name, "chainhop", NULL); // KDLOG
					}
					gj_chain_on[slot] = true;
					gj_chain_flip[slot] = 0;
					gj_chain_flew[slot] = false;
					wish[0] = self->s.v.velocity[0] / vh;
					wish[1] = self->s.v.velocity[1] / vh;
					wish[2] = 0;
					view_yaw = vectoyaw(wish);
					VectorSet(ang, 0, view_yaw, 0);
					trap_makevectors(ang);
					self->fb.desired_angle[PITCH] = 0;
					self->fb.desired_angle[YAW] = view_yaw;
					self->fb.desired_angle[ROLL] = 0;
					direction[0] = DotProduct(g_globalvars.v_forward, wish) * 800;
					direction[1] = DotProduct(g_globalvars.v_right, wish) * 800;
					direction[2] = 0;
					*jumping = true;
					*firing = false;
					*impulse = 0;
					return true;
				}
			}
		}
	}

	// Drive carrot: a point on the corridor line, ahead toward the lip but never
	// past it (ca clamped <= 0 so we never steer into the gap), and biased SOUTH of
	// the line (into the open corridor, away from the pillar) by south_bias so the
	// bot arrives south-of-line where the crossing is clear. Steering at this point
	// converges the bot onto the (biased) corridor AND forward to the lip.
	{
		float south_bias = cvar("k_kbot_gj_south_bias");

		if (south_bias < 0) { south_bias = 0; }
		if (lane == 4) { south_bias = 0; } // no pillar to bias away from
		ca = along_u + lookahead;
		if (ca > 0) { ca = 0; }
		tgt[0] = take[0] + u[0] * ca;
		tgt[1] = take[1] + u[1] * ca - south_bias; // bias toward world -Y (south)
		tgt[2] = org[2];
		// E12b BUILD ORBIT (lane 4 + chain): bots arrive at the lip carrying
		// 420-430 with no runway left (traj: engage along -69..-95, vh 423-431,
		// and the lip-carrot 60-deg build arc ran one straight off the EAST edge
		// at x1472 > deck edge 1456). Hold the drive carrot at a deep-runway
		// point (~chain_back behind the lip -- deck floor spans along 0..-530,
		// edges 130u+ from the orbit) until the bot carries chain-exit speed;
		// then the normal lip carrot pulls it into a straight inbound dash whose
		// heading converges on the launch box, and the chain trigger above fires
		// at d ~= one hop out. Bots already deeper than the orbit keep the
		// normal inbound carrot (build happens en route).
		if ((lane == 4) && cvar("k_kbot_gj_chain"))
		{
			float cback = cvar("k_kbot_gj_chain_back");
			float cexit = cvar("k_kbot_gj_chain_exit");

			if (cback <= 0) { cback = 340; }
			if (cexit <= 0) { cexit = 430; }
			if ((vh < cexit) && (along_u > -(cback - 64)))
			{
				// Bias the orbit 40u toward the open (negative-lat) side: a
				// centered orbit's loops grazed the WEST deck edge x1264
				// (probe: ABORT_TIMEOUT stuck at vh 27 by the edge).
				tgt[0] = take[0] + u[0] * (-cback) - (-u[1]) * 40;
				tgt[1] = take[1] + u[1] * (-cback) - u[0] * 40;
				tgt[2] = org[2];
			}
		}
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
	// E12 RL lane: ANGLE SCHEDULE. The ground-accelerate cap is
	// wishspeed/cos(angle) (42 deg -> ~430, 60 deg -> ~640), but a steep
	// angle from low speed stalls completely (measured: 62 deg flat ->
	// ABORT_TIMEOUT at vh 14-22). So build flat (46 deg, fast) until the
	// mid-range, then steepen (rl_bangle, default 60) for the top-up past
	// the 430 wall toward the slot window (~459+).
	if (lane == 4)
	{
		float rb = cvar("k_kbot_gj_rl_bangle");

		build_angle = (vh < 400) ? 46 : ((rb > 0) ? rb : 60);
	}
	// OPTION-2 approach speed-build (E11-first increment). The launch floor
	// (vreq*mul ~413) is where the bot is ALLOWED to launch; but on the north-
	// bowed mirror lanes the air-carve scrubs ~10-15 ups, so a bot arriving AT
	// the floor lands short (~40% pit-fall, findings 07-04). Instead of raising
	// the floor (which collapsed launch frequency 32->2 without better land-rate)
	// keep circle-building PAST the floor up to appcarve_target*floor (~425) in
	// the runway BEHIND the launch window (at along_u < -launch_win the launch
	// gate above hasn't fired), so the bot ARRIVES hot and the scrub still leaves
	// it clearing the void. Lane-scoped to 2/3 + mirrorcarve, combat-gated
	// (enemy_visible yields above), cvar-gated default 0 for A/B. When off (or
	// lanes 0/1) build_hi==floor so (vh<build_hi) is byte-identical to !fast.
	{
		float build_hi = floor;

		if (cvar("k_kbot_gj_appcarve") && cvar("k_kbot_gj_mirrorcarve") &&
			(lane == 2 || lane == 3))
		{
			float tgt_mul = cvar("k_kbot_gj_appcarve_target");
			float ba = cvar("k_kbot_gj_appcarve_angle");

			build_hi = floor * ((tgt_mul > 0) ? tgt_mul : 1.06f);
			if (ba > 0) { build_angle = ba; }   // steeper carve builds faster
		}
		// E12 RL lane: build to the MIDDLE of the slot speed window (not the
		// floor) on the long bridge-deck runway -- the launch-aim snaps the
		// velocity direction at the hop, so approach-line curvature from the
		// circle-build is harmless here (unlike the mirror-lane appcarve,
		// where the free launch line was the binding constraint).
		if (lane == 4)
		{
			float rm = cvar("k_kbot_gj_rl_mul");
			float rx = cvar("k_kbot_gj_rl_max");

			build_hi = vreq * 0.5f * (((rm > 0) ? rm : 0.99f)
									  + ((rx > 0) ? rx : 1.09f));
		}
		if (cvar("k_kbot_gj_app_build") && onground && (vh < build_hi) &&
			along_u < 0)
		{
			vec3_t cur;
			cur[0] = self->s.v.velocity[0];
			cur[1] = self->s.v.velocity[1];
			cur[2] = 0;

			if (VectorLength(cur) > 40)   // need real velocity to circle off
			{
				float cyaw = atan2(cur[1], cur[0]) * 180.0f / M_PI;
				float e = vectoyaw(wish) - cyaw;
				int sgn;

				while (e > 180) { e -= 360; }
				while (e < -180) { e += 360; }
				sgn = (e >= 0) ? 1 : -1;
				// E12b: during a turnaround (carrot more than ~100 deg off the
				// velocity -- the orbit flip) do NOT apply the build rotation:
				// rotating the wish off an already-backward carrot widened the
				// turn arc clear across the lip (probe: declines at lat 70-170,
				// aerr 93-177). A plain wish gives the tightest friction turn;
				// the build resumes once roughly heading at the carrot again.
				if ((lane == 4) && cvar("k_kbot_gj_chain") &&
					((e > 100) || (e < -100)))
				{
					// keep wish as-is (plain steer at the carrot)
				}
				else
				{
					VectorNormalize(cur);
					RotatePointAroundVector(wish, up, cur, build_angle * sgn);
					wish[2] = 0;
					VectorNormalize(wish);
				}
			}
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

		// Owner rule: never even start the RL setup while an enemy can see us.
		if ((pick == 4) && cvar("k_kbot_gj_rl_unseen") && GJ_SeenByEnemy(self))
		{
			pick = -1;
		}

		if (pick >= 0)
		{
			gj_lane_active[slot] = pick;
			gj_app_t0[slot] = now;
			gj_build_sign[slot] = 0;
			gj_chain_on[slot] = false;
			gj_state[slot] = GJ_APPROACH;
			if (cvar("k_kbot_gj_gatelog"))
			{
				G_cprint("[gapjump] lane=%s result=APP_ENGAGE\n",
						 gj_lanes[pick].name);
			}
			KDLog_Play(self, gj_lanes[pick].name, "engage", NULL); // KDLOG
			return GJ_ApproachFrame(self, slot, pick, jumping, firing, impulse,
									direction);
		}

		// ---- E10 STAGE (k_kbot_gj_route): goal is across the gap but we're
		// outside the tight intent box. Walk toward the staging point on the
		// lane line 160u behind the lip; once inside the box the APP_ENGAGE
		// branch above takes over. Combat-yield already handled (enemy_visible
		// bail earlier in this function) -- no movement override in a fight.
		if (cvar("k_kbot_gj_route"))
		{
			int rlane = GJ_PickRouteLane(self);

			// Owner rule: don't stage toward the RL deck while observed either.
			if ((rlane == 4) && cvar("k_kbot_gj_rl_unseen") && GJ_SeenByEnemy(self))
			{
				rlane = -1;
			}

			if (rlane >= 0)
			{
				vec3_t take, land, u, sp, wish;
				float fail_z, lanelen;

				GJ_Geometry(rlane, take, land, &fail_z);
				u[0] = land[0] - take[0];
				u[1] = land[1] - take[1];
				u[2] = 0;
				lanelen = VectorLength(u);
				if (lanelen >= 1)
				{
					VectorNormalize(u);
					VectorMA(take, -160, u, sp); // staging point behind the lip
					sp[2] = take[2];
					VectorSubtract(sp, self->s.v.origin, wish);
					wish[2] = 0;
					if (VectorLength(wish) > 24)
					{
						VectorNormalize(wish);
						VectorScale(wish, 320, direction);
						direction[2] = 0;
						*jumping = false;
						if (!gj_stage_on[slot] && cvar("k_kbot_gj_gatelog"))
						{
							G_cprint("[gjstage] lane=%s slot=%d engage\n",
									 gj_lanes[rlane].name, slot);
						}
						gj_stage_on[slot] = true;
						return true;
					}
				}
			}
			else if (gj_stage_on[slot])
			{
				gj_stage_on[slot] = false;
				if (cvar("k_kbot_gj_gatelog"))
				{
					G_cprint("[gjstage] slot=%d release\n", slot);
				}
			}
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
