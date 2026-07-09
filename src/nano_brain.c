/*
 nano_brain.c -- nano-bots S2a movement brain (rtx port).

 Sense adapter, item-goal selection, navmesh route follow, steering, aim spring,
 and bot-command emission. All per-bot state lives in g_nano_bots[], never in
 gedict_t. Compiled only when NANO_SUPPORT is ON.
*/
#ifdef NANO_SUPPORT

#include "g_local.h"
#include "nano_brain.h"

#include <math.h>

static nano_bot_t g_nano_bots[NANO_MAX_SLOTS];

// ---------------------------------------------------------------------------
// First-frame init for a slot.
// ---------------------------------------------------------------------------
static void Nano_BotInit(nano_bot_t *bot, const nano_sense_t *s)
{
	memset(bot, 0, sizeof(*bot));
	bot->air_leg = -1;
	bot->goal_cell = -1;
	bot->goal_ent = -1;
	VectorCopy(s->v_angle, bot->aim);
	VectorClear(bot->aim_vel);
	VectorCopy(s->origin, bot->stuck_origin);
	bot->stuck_since = s->now;
	bot->progress_best = 999999.0f;
	bot->initialized = true;
}

// ---------------------------------------------------------------------------
// Sense adapter: copy the edict/engine state we need into an rtx-like Sense.
// ---------------------------------------------------------------------------
static void Nano_BrainSense(gedict_t *self, nano_sense_t *s)
{
	s->now = g_globalvars.time;
	s->frametime = g_globalvars.frametime;
	s->msec = (int)(s->frametime * 1000.0f);
	if (s->msec < 1)
	{
		s->msec = 1;
	}
	else if (s->msec > 100)
	{
		s->msec = 100;
	}

	VectorCopy(self->s.v.origin, s->origin);
	VectorCopy(self->s.v.v_angle, s->v_angle);
	s->client = NUM_FOR_EDICT(self);
	s->weapon = (int)self->s.v.weapon;
	s->on_ground = ((int)self->s.v.flags & FL_ONGROUND) != 0;
	s->alive = self->s.v.health > 0.0f && self->s.v.deadflag == 0.0f;
	s->vz = self->s.v.velocity[2];
	s->speed = sqrtf(self->s.v.velocity[0] * self->s.v.velocity[0]
					+ self->s.v.velocity[1] * self->s.v.velocity[1]);
	s->has_rl = ((int)self->s.v.items & IT_ROCKET_LAUNCHER) != 0;
	s->ammo_rockets = self->s.v.ammo_rockets;
	s->health = self->s.v.health;
	s->armortype = self->s.v.armortype;
	s->armorvalue = self->s.v.armorvalue;
	s->quad = self->super_damage_finished > s->now;
}

// ---------------------------------------------------------------------------
// Goal desire constants (S2a: fixed, simple).
// ---------------------------------------------------------------------------
static float Nano_ItemDesire(int tp_flags)
{
	if (tp_flags & (it_quad | it_pent | it_ring | it_suit))
	{
		return 250.0f;
	}
	if (tp_flags & (it_ra | it_ya | it_rl | it_lg | it_mh))
	{
		return 100.0f;
	}
	if (tp_flags & (it_ga | it_health))
	{
		return 40.0f;
	}
	if (tp_flags & (it_shells | it_nails | it_rockets | it_cells))
	{
		return 10.0f;
	}
	return 0.0f;
}

// ---------------------------------------------------------------------------
// Pick a reachable item goal. Costs must already be flooded from bot_cell.
// ---------------------------------------------------------------------------
static void Nano_PickGoal(nano_bot_t *bot, const nano_sense_t *s,
						  const nano_navgraph_t *g, int bot_cell, qbool force)
{
	int n = Nano_NavNumCells(g);
	float *costs = NULL;
	gedict_t *ent;
	float best_score = 0.0f;
	int best_cell = -1;
	int best_ent = -1;
	int goal_mask = it_ra | it_ya | it_ga | it_mh | it_quad | it_pent | it_ring
					| it_rl | it_lg | it_health | it_rockets | it_cells | it_shells | it_nails;

	if (!force && bot->goal_ent > 0 && s->now < bot->goal_select_time)
	{
		return;
	}

	costs = (float *)malloc((size_t)n * sizeof(float));
	if (!costs)
	{
		return;
	}
	if (!Nano_NavCostsFrom(g, bot_cell, costs, n))
	{
		free(costs);
		return;
	}

	for (ent = world; (ent = nextent(ent));)
	{
		float desire, t, score;
		int item_cell;
		qbool is_current;

		if (!(ent->tp_flags & goal_mask))
		{
			continue;
		}
		if (ent->s.v.solid != SOLID_TRIGGER)
		{
			continue;
		}

		desire = Nano_ItemDesire(ent->tp_flags);
		if (desire <= 0.0f)
		{
			continue;
		}

		item_cell = Nano_NavNearest(g, ent->s.v.origin);
		if (item_cell < 0 || item_cell >= n || costs[item_cell] >= NANO_NAV_UNREACHABLE)
		{
			continue;
		}

		t = costs[item_cell];
		score = desire / (t + 1.0f);

		is_current = (NUM_FOR_EDICT(ent) == bot->goal_ent);
		if (is_current && (s->now - bot->goal_since) < 3.0f)
		{
			score *= 1.3f;
		}

		if (score > best_score)
		{
			best_score = score;
			best_cell = item_cell;
			best_ent = NUM_FOR_EDICT(ent);
		}
	}

	// Fallback: nearest reachable cell (keeps the bot moving even with no items).
	if (best_cell < 0)
	{
		best_cell = Nano_NavNearestReachable(g, s->origin, costs, n);
		best_ent = -1;
	}

	free(costs);

	if (best_cell >= 0
		&& (best_ent != bot->goal_ent || best_cell != bot->goal_cell))
	{
		bot->goal_cell = best_cell;
		bot->goal_ent = best_ent;
		bot->goal_since = s->now;
		bot->repath_time = s->now;
	}

	bot->goal_select_time = s->now + NANO_GOAL_SELECT_INTERVAL;
}

// ---------------------------------------------------------------------------
// Re-path / advance along the current route.
// ---------------------------------------------------------------------------
static void Nano_FollowRoute(nano_bot_t *bot, const nano_sense_t *s,
							 const nano_navgraph_t *g, int bot_cell)
{
	qbool repath = false;

	if (bot->route_len <= 0 || bot->goal_cell < 0)
	{
		repath = true;
	}
	else if (bot->route[bot->route_len - 1] >= Nano_NavNumLinks(g)
			 || Nano_NavLinkTarget(g, bot->route[bot->route_len - 1]) != bot->goal_cell)
	{
		// route destination no longer matches goal
		repath = true;
	}
	else if (s->now >= bot->repath_time)
	{
		repath = true;
	}

	if (repath && bot->goal_cell >= 0)
	{
		int rl = Nano_NavFindPath(g, bot_cell, bot->goal_cell,
								  bot->route, NANO_MAX_ROUTE);
		if (rl >= 0)
		{
			bot->route_len = rl;
			bot->route_pos = 0;
		}
		else
		{
			bot->route_len = 0;
			bot->route_pos = 0;
		}
		bot->repath_time = s->now + NANO_REPATH_INTERVAL;
	}

	// Advance route_pos while within arrive radius of the current link target.
	while (bot->route_pos < bot->route_len)
	{
		int link = bot->route[bot->route_pos];
		const float *target = Nano_NavCellOrigin(g, Nano_NavLinkTarget(g, link));
		float dx, dy, r2;

		if (!target)
		{
			break;
		}

		dx = target[0] - s->origin[0];
		dy = target[1] - s->origin[1];
		r2 = NANO_ARRIVE_RADIUS * NANO_ARRIVE_RADIUS;
		if (dx * dx + dy * dy <= r2)
		{
			bot->route_pos++;
		}
		else
		{
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// Air commitment latch for jump-gap legs.
// ---------------------------------------------------------------------------
static void Nano_AirCommit(nano_bot_t *bot, const nano_sense_t *s,
						   const nano_navgraph_t *g)
{
	int cur_link = -1;
	int cur_kind = -1;
	qbool on_jump_leg = false;

	if (bot->route_pos < bot->route_len)
	{
		cur_link = bot->route[bot->route_pos];
		cur_kind = Nano_NavLinkKind(g, cur_link);
	}
	on_jump_leg = (cur_kind == NANO_LINK_JUMPGAP || cur_kind == NANO_LINK_DJUMP);

	if (on_jump_leg && bot->air_leg != cur_link)
	{
		bot->air_leg = cur_link;
		bot->air_started = s->now;
	}

	if (bot->air_leg >= 0)
	{
		if (!on_jump_leg
			|| (s->on_ground && (s->now - bot->air_started) > NANO_AIR_COMMIT_GRACE))
		{
			// Landed or advanced off the jump leg.
			bot->air_leg = -1;
		}
		else if ((s->now - bot->air_started) > NANO_AIR_COMMIT_MAX)
		{
			// Timeout: abandon the leg and re-path.
			bot->air_leg = -1;
			bot->route_len = 0;
			bot->route_pos = 0;
			bot->repath_time = s->now;
		}
	}
}

// ---------------------------------------------------------------------------
// Stuck and progress watchdogs. Suspended while air-committed.
// ---------------------------------------------------------------------------
static qbool Nano_StuckCheck(nano_bot_t *bot, const nano_sense_t *s)
{
	float dist;
	vec3_t origin;

	if (bot->air_leg >= 0)
	{
		return false;
	}

	VectorCopy(s->origin, origin);
	dist = VectorDistance(origin, bot->stuck_origin);
	if (dist > NANO_STUCK_MOVE)
	{
		VectorCopy(s->origin, bot->stuck_origin);
		bot->stuck_since = s->now;
		return false;
	}

	if ((s->now - bot->stuck_since) > NANO_STUCK_TIME)
	{
		bot->stuck_since = s->now;
		bot->route_len = 0;
		bot->repath_time = s->now;
		return true;
	}

	return false;
}

static void Nano_ProgressCheck(nano_bot_t *bot, const nano_sense_t *s,
							   const nano_navgraph_t *g)
{
	const float *goal_origin;
	float goal_dist;

	if (bot->air_leg >= 0 || bot->goal_cell < 0)
	{
		return;
	}

	goal_origin = Nano_NavCellOrigin(g, bot->goal_cell);
	if (!goal_origin)
	{
		return;
	}

	{
		vec3_t goalv, origin;
		VectorCopy(goal_origin, goalv);
		VectorCopy(s->origin, origin);
		goal_dist = VectorDistance(origin, goalv);
	}

	if (goal_dist < bot->progress_best - NANO_PROGRESS_EPS)
	{
		bot->progress_best = goal_dist;
		bot->progress_since = s->now;
	}
	else if ((s->now - bot->progress_since) > NANO_PROGRESS_TIME)
	{
		bot->route_len = 0;
		bot->repath_time = s->now;
		bot->progress_best = goal_dist;
		bot->progress_since = s->now;
	}
}

// ---------------------------------------------------------------------------
// Steering: choose waypoint, look-ahead, and jump state.
// ---------------------------------------------------------------------------
static void Nano_Steer(nano_bot_t *bot, const nano_sense_t *s,
					   const nano_navgraph_t *g, vec3_t out_look,
					   vec3_t out_move, int *out_buttons)
{
	const float *waypoint = NULL;
	const float *look = NULL;
	int kind = -1;
	vec3_t to_wp;
	float yaw;

	VectorClear(out_look);
	VectorClear(out_move);
	*out_buttons = 0;

	if (bot->route_pos < bot->route_len)
	{
		int link = bot->route[bot->route_pos];
		kind = Nano_NavLinkKind(g, link);
		waypoint = Nano_NavCellOrigin(g, Nano_NavLinkTarget(g, link));
	}
	else if (bot->goal_cell >= 0)
	{
		waypoint = Nano_NavCellOrigin(g, bot->goal_cell);
	}

	if (!waypoint)
	{
		return;
	}

	// Look-ahead: two legs ahead if available, else waypoint.
	if (bot->route_pos + 2 < bot->route_len)
	{
		int link = bot->route[bot->route_pos + 2];
		look = Nano_NavCellOrigin(g, Nano_NavLinkTarget(g, link));
	}
	if (!look)
	{
		look = waypoint;
	}

	VectorSubtract(waypoint, s->origin, to_wp);
	yaw = atan2f(to_wp[1], to_wp[0]) * 180.0f / (float)M_PI;
	out_look[0] = 0.0f;
	out_look[1] = yaw;
	out_look[2] = 0.0f;

	// World-space move direction (XY only).
	{
		float len = sqrtf(to_wp[0] * to_wp[0] + to_wp[1] * to_wp[1]);
		if (len > 1.0f)
		{
			out_move[0] = (to_wp[0] / len) * NANO_MOVE_SPEED;
			out_move[1] = (to_wp[1] / len) * NANO_MOVE_SPEED;
		}
		out_move[2] = 0.0f;
	}

	// Jump on takeoff for jump-gap legs.
	if (s->on_ground && kind == NANO_LINK_JUMPGAP)
	{
		*out_buttons |= NANO_BUTTON_JUMP;
	}
}

// ---------------------------------------------------------------------------
// Aim spring: smooth view toward the look target.
// ---------------------------------------------------------------------------
static void Nano_AimSpring(nano_bot_t *bot, const nano_sense_t *s,
						   const vec3_t look)
{
	float skill = cvar("k_nano_skill");
	float omega;

	if (skill < 0.0f)
	{
		skill = 0.0f;
	}
	else if (skill > 7.0f)
	{
		skill = 7.0f;
	}
	omega = 6.0f + skill * 2.0f;

	Nano_AimSpringStep(bot, look, omega, s->frametime);
}

// ---------------------------------------------------------------------------
// Emit the final bot command through KTX's engine syscall.
// ---------------------------------------------------------------------------
static void Nano_EmitCmd(nano_bot_t *bot, const nano_sense_t *s,
						 const vec3_t move_world, int buttons)
{
	vec3_t vf, vr, vu;
	vec3_t dir;
	float len;
	int forwardmove, sidemove;

	AngleVectors(bot->aim, vf, vr, vu);

	VectorCopy(move_world, dir);
	dir[2] = 0.0f;
	len = VectorLength(dir);
	if (len > 1.0f)
	{
		VectorNormalize(dir);
	}
	else if (len <= 0.0f)
	{
		VectorClear(dir);
	}

	forwardmove = (int)(DotProduct(vf, dir) * NANO_MOVE_SPEED);
	sidemove = (int)(DotProduct(vr, dir) * NANO_MOVE_SPEED);

	trap_SetBotCMD(s->client, s->msec,
				   bot->aim[0], bot->aim[1], bot->aim[2],
				   forwardmove, sidemove, 0, buttons, 0);
}

// ---------------------------------------------------------------------------
// Per-frame brain entry point.
// ---------------------------------------------------------------------------
qbool Nano_BrainFrame(gedict_t *self)
{
	int ent;
	const nano_navgraph_t *g;
	nano_bot_t *bot;
	nano_sense_t s;
	int bot_cell;
	vec3_t look, move_world;
	int buttons;
	qbool stuck_jump;
	qbool force_repick;

	ent = NUM_FOR_EDICT(self);
	if (ent < 0 || ent >= NANO_MAX_SLOTS)
	{
		return false;
	}

	Nano_BrainSense(self, &s);
	if (!s.alive)
	{
		return false; // let Frogbot handle respawn
	}

	g = Nano_NavGraph();
	if (!g)
	{
		return false;
	}

	bot = &g_nano_bots[ent];
	if (!bot->initialized)
	{
		Nano_BotInit(bot, &s);
	}

	bot_cell = Nano_NavNearest(g, s.origin);
	if (bot_cell < 0)
	{
		// No cell: stand still but keep the frame.
		trap_SetBotCMD(s.client, s.msec,
					   s.v_angle[0], s.v_angle[1], s.v_angle[2],
					   0, 0, 0, 0, 0);
		return true;
	}

	// Decide whether to re-pick the goal.
	force_repick = (bot->goal_cell < 0);
	if (!force_repick && bot->goal_ent > 0)
	{
		gedict_t *goal_ent = &g_edicts[bot->goal_ent];
		if (!(goal_ent->tp_flags) || goal_ent->s.v.solid != SOLID_TRIGGER)
		{
			force_repick = true;
		}
	}
	if ((s.now - bot->goal_since) > NANO_GOAL_GIVEUP_TIME)
	{
		force_repick = true;
	}

	Nano_PickGoal(bot, &s, g, bot_cell, force_repick);

	if (bot->air_leg < 0)
	{
		Nano_FollowRoute(bot, &s, g, bot_cell);
	}

	Nano_AirCommit(bot, &s, g);
	stuck_jump = Nano_StuckCheck(bot, &s);
	Nano_ProgressCheck(bot, &s, g);

	Nano_Steer(bot, &s, g, look, move_world, &buttons);
	if (stuck_jump)
	{
		buttons |= NANO_BUTTON_JUMP;
	}

	Nano_AimSpring(bot, &s, look);
	Nano_EmitCmd(bot, &s, move_world, buttons);

	return true;
}

#endif // NANO_SUPPORT
