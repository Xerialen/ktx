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
// Reset a slot's brain state (public so Nano_ClearMark can call it).
// ---------------------------------------------------------------------------
void Nano_BrainClearSlot(int ent)
{
	nano_bot_t *bot;

	if (ent < 0 || ent >= NANO_MAX_SLOTS)
	{
		return;
	}

	bot = &g_nano_bots[ent];
	memset(bot, 0, sizeof(*bot));
	bot->air_leg = -1;
	bot->goal_cell = -1;
	bot->goal_ent = -1;
	bot->enemy_ent = -1;
	bot->enemy_seen_time = -999999.0f;
	bot->initialized = false;
}

// ---------------------------------------------------------------------------
// First-frame init for a slot.
// ---------------------------------------------------------------------------
static void Nano_BotInit(nano_bot_t *bot, const nano_sense_t *s)
{
	memset(bot, 0, sizeof(*bot));
	bot->air_leg = -1;
	bot->goal_cell = -1;
	bot->goal_ent = -1;
	bot->enemy_ent = -1;
	bot->enemy_seen_time = -999999.0f;
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
	char *teamstr;

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

	// S3a combat fields.
	s->items = (int)self->s.v.items;
	s->ammo_shells = (int)self->s.v.ammo_shells;
	s->ammo_nails = (int)self->s.v.ammo_nails;
	s->ammo_cells = (int)self->s.v.ammo_cells;
	s->has_lg = (s->items & IT_LIGHTNING) != 0;
	s->has_ssg = (s->items & IT_SUPER_SHOTGUN) != 0;
	s->has_sng = (s->items & IT_SUPER_NAILGUN) != 0;
	s->has_gl = (s->items & IT_GRENADE_LAUNCHER) != 0;
	s->view_height = self->s.v.view_ofs[2];
	if (s->view_height <= 0.0f)
	{
		s->view_height = NANO_EYE_HEIGHT;
	}

	teamstr = getteam(self);
	s->team = teamstr ? (int)teamstr[0] : 0;
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

	// Aim toward the look-ahead point for smoother corridor tracking.
	{
		vec3_t to_look;
		VectorSubtract(look, s->origin, to_look);
		yaw = atan2f(to_look[1], to_look[0]) * 180.0f / (float)M_PI;
		out_look[0] = 0.0f;
		out_look[1] = yaw;
		out_look[2] = 0.0f;
	}

	// World-space move direction (XY only) uses the immediate waypoint.
	VectorSubtract(waypoint, s->origin, to_wp);
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
// S3a combat: perception, aim-leading, weapon selection, firing.
// ---------------------------------------------------------------------------

// Convert a Quake weapon impulse (1..8) to the corresponding IT_* bit.
static int Nano_ItemBitForImpulse(int impulse)
{
	switch (impulse)
	{
		case 1: return IT_AXE;
		case 2: return IT_SHOTGUN;
		case 3: return IT_SUPER_SHOTGUN;
		case 4: return IT_NAILGUN;
		case 5: return IT_SUPER_NAILGUN;
		case 6: return IT_GRENADE_LAUNCHER;
		case 7: return IT_ROCKET_LAUNCHER;
		case 8: return IT_LIGHTNING;
		default: return 0;
	}
}

static float Nano_CombatEffectiveFOV(void)
{
	float skill = cvar("k_nano_skill");
	float base = cvar("k_nano_fov");
	float fov;

	skill = bound(0, skill, 7);
	if (base <= 0.0f)
	{
		base = NANO_FOV_BASE;
	}
	fov = base + skill * 4.0f;
	if (fov > 360.0f)
	{
		fov = 360.0f;
	}
	return fov;
}

static float Nano_CombatReaction(void)
{
	float skill = cvar("k_nano_skill");
	float base = cvar("k_nano_reaction");
	float r;

	skill = bound(0, skill, 7);
	if (base <= 0.0f)
	{
		base = NANO_REACTION_BASE;
	}
	r = base * (1.0f - skill / 8.0f);
	if (r < 0.05f)
	{
		r = 0.05f;
	}
	return r;
}

static qbool Nano_CombatSameTeam(const nano_sense_t *s, gedict_t *other)
{
	char *oteam;

	if (!other)
	{
		return false;
	}

	oteam = getteam(other);
	if (!oteam || !oteam[0])
	{
		return false; // no team in non-teamplay: everyone is an enemy
	}

	return s->team != 0 && s->team == (int)oteam[0];
}

static qbool Nano_CombatVisible(const nano_sense_t *s, gedict_t *target, vec3_t out_target_pos)
{
	vec3_t eye, tgt;
	int self_num;
	gedict_t *hit;
	float dz;

	if (!target || target->s.v.health <= 0)
	{
		return false;
	}

	self_num = s->client;

	VectorCopy(s->origin, eye);
	eye[2] += s->view_height;

	// Aim at the enemy's mid-body; try head if the first trace hits a non-vital bbox point.
	VectorCopy(target->s.v.origin, tgt);
	dz = (target->s.v.maxs[2] - target->s.v.mins[2]) * 0.5f;
	if (dz < 8.0f)
	{
		dz = 16.0f;
	}
	tgt[2] += dz;

	trap_traceline(eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2], false, self_num);
	if (g_globalvars.trace_fraction >= 1.0f)
	{
		VectorCopy(tgt, out_target_pos);
		return true;
	}

	hit = PROG_TO_EDICT(g_globalvars.trace_ent);
	if (hit == target)
	{
		VectorCopy(tgt, out_target_pos);
		return true;
	}

	// Retry at head height.
	tgt[2] = target->s.v.origin[2] + target->s.v.view_ofs[2];
	trap_traceline(eye[0], eye[1], eye[2], tgt[0], tgt[1], tgt[2], false, self_num);
	if (g_globalvars.trace_fraction >= 1.0f || PROG_TO_EDICT(g_globalvars.trace_ent) == target)
	{
		VectorCopy(tgt, out_target_pos);
		return true;
	}

	return false;
}

static void Nano_CombatUpdate(nano_bot_t *bot, const nano_sense_t *s)
{
	gedict_t *ent;
	gedict_t *best = NULL;
	float best_score = -1.0f;
	float fov = Nano_CombatEffectiveFOV();
	float reaction = Nano_CombatReaction();
	vec3_t visible_pos;
	qbool current_visible = false;

	// 1. Update current enemy visibility first so memory decays correctly.
	if (bot->enemy_ent > 0)
	{
		gedict_t *current = &g_edicts[bot->enemy_ent];
		if (!ISLIVE(current)
			|| (bot->enemy_seen_time > 0 && (s->now - bot->enemy_seen_time) > NANO_MEMORY_TIME))
		{
			bot->enemy_ent = -1;
			bot->enemy_visible = false;
		}
		else
		{
			current_visible = Nano_CombatVisible(s, current, visible_pos);
			if (current_visible)
			{
				VectorCopy(visible_pos, bot->enemy_pos);
				bot->enemy_seen_time = s->now;
				if (!bot->enemy_visible)
				{
					bot->enemy_visible_since = s->now;
				}
				bot->enemy_visible = true;
			}
			else
			{
				bot->enemy_visible = false;
			}
		}
	}

	// 2. Search for a better / first target.
	for (ent = world; (ent = nextent(ent));)
	{
		vec3_t pos;
		float dist, score;
		qbool visible;

		if (ent == &g_edicts[s->client])
		{
			continue;
		}
		if (!ent->isBot && ent->ct != ctPlayer)
		{
			continue;
		}
		if (!ISLIVE(ent))
		{
			continue;
		}
		if (Nano_CombatSameTeam(s, ent))
		{
			continue;
		}

		VectorSubtract(ent->s.v.origin, s->origin, pos);
		dist = VectorLength(pos);
		if (dist > 3000.0f)
		{
			continue;
		}

		if (!Nano_InFOV(bot->aim, s->origin, ent->s.v.origin, fov))
		{
			continue;
		}

		visible = Nano_CombatVisible(s, ent, pos);
		if (!visible)
		{
			continue;
		}

		// Prefer the current enemy (hysteresis), then closest visible threat.
		score = 1.0f / (dist + 1.0f);
		if (NUM_FOR_EDICT(ent) == bot->enemy_ent)
		{
			score *= 1.5f;
		}

		if (score > best_score)
		{
			best_score = score;
			best = ent;
			VectorCopy(pos, visible_pos);
		}
	}

	// 3. Promote best target through reaction delay.
	if (best)
	{
		int best_num = NUM_FOR_EDICT(best);

		if (bot->enemy_ent != best_num)
		{
			// New candidate: start reacting.
			bot->enemy_ent = best_num;
			bot->enemy_reacted_time = s->now + reaction;
			bot->enemy_visible_since = s->now;
			if (cvar("k_nano_debug") >= 2)
			{
				G_cprint("[nano] combat new target slot=%d enemy=%d\n", s->client, best_num);
			}
		}
		else if (s->now >= bot->enemy_reacted_time)
		{
			// Fully reacted: refresh state.
			bot->enemy_visible = true;
			bot->enemy_seen_time = s->now;
			VectorCopy(visible_pos, bot->enemy_pos);
			if (cvar("k_nano_debug") >= 2)
			{
				G_cprint("[nano] combat active slot=%d enemy=%d\n", s->client, best_num);
			}
		}
	}
}

static qbool Nano_CombatActive(const nano_bot_t *bot, const nano_sense_t *s)
{
	if (bot->enemy_ent <= 0)
	{
		return false;
	}
	if (s->now < bot->enemy_reacted_time)
	{
		return false;
	}
	if (bot->enemy_seen_time > 0 && (s->now - bot->enemy_seen_time) > NANO_MEMORY_TIME)
	{
		return false;
	}
	return true;
}

static void Nano_CombatAim(nano_bot_t *bot, const nano_sense_t *s, vec3_t out_look)
{
	gedict_t *enemy;
	vec3_t eye, to, lead;
	float yaw, pitch;
	float dist;
	qbool use_rocket;

	VectorClear(out_look);

	if (!Nano_CombatActive(bot, s))
	{
		return;
	}

	enemy = &g_edicts[bot->enemy_ent];
	if (!ISLIVE(enemy))
	{
		return;
	}

	VectorCopy(s->origin, eye);
	eye[2] += s->view_height;

	// Decide whether to lead: rockets and grenades against a moving target.
	dist = VectorDistance(eye, bot->enemy_pos);
	use_rocket = (s->has_rl && s->ammo_rockets > 0 && dist <= NANO_ROCKET_RANGE)
				 || (s->has_gl && s->ammo_rockets > 0);

	if (use_rocket)
	{
		Nano_LeadAim(eye, bot->enemy_pos, enemy->s.v.velocity, NANO_ROCKET_SPEED, lead);
		VectorCopy(lead, to);
	}
	else
	{
		VectorSubtract(bot->enemy_pos, eye, to);
	}

	yaw = atan2f(to[1], to[0]) * 180.0f / (float)M_PI;
	// Quake/KTX pitch convention: positive pitch looks down, so negate atan2(z, xy).
	pitch = -atan2f(to[2], sqrtf(to[0] * to[0] + to[1] * to[1])) * 180.0f / (float)M_PI;

	out_look[0] = pitch;
	out_look[1] = yaw;
	out_look[2] = 0.0f;
}

static int Nano_CombatWeapon(nano_bot_t *bot, const nano_sense_t *s)
{
	gedict_t *enemy;
	float dist;
	int desired;
	int desired_bit;

	if (!Nano_CombatActive(bot, s))
	{
		return 0;
	}

	if (s->now < bot->weapon_switch_time)
	{
		return 0;
	}

	enemy = &g_edicts[bot->enemy_ent];
	if (!ISLIVE(enemy))
	{
		return 0;
	}

	{
		vec3_t delta;
		VectorSubtract(s->origin, enemy->s.v.origin, delta);
		dist = VectorLength(delta);
	}

	desired = Nano_WeaponForRange(dist, s->items, s->ammo_shells, s->ammo_nails,
								s->ammo_rockets, s->ammo_cells);
	if (desired <= 0)
	{
		return 0;
	}

	desired_bit = Nano_ItemBitForImpulse(desired);
	if ((s->weapon & desired_bit) == desired_bit)
	{
		return 0; // already wielding
	}

	bot->weapon_switch_time = s->now + NANO_WEAPON_SWITCH_TIME;
	return desired;
}

static qbool Nano_CombatShouldFire(nano_bot_t *bot, const nano_sense_t *s)
{
	gedict_t *enemy;
	vec3_t eye, aim_dir, to_enemy;
	float yaw_diff, pitch_diff, tol;
	float skill;

	if (!Nano_CombatActive(bot, s) || !bot->enemy_visible)
	{
		return false;
	}

	enemy = &g_edicts[bot->enemy_ent];
	if (!ISLIVE(enemy))
	{
		return false;
	}

	skill = bound(0, cvar("k_nano_skill"), 7);
	tol = NANO_FIRE_TOL_BASE + (7.0f - skill) * 2.0f;

	// Horizontal alignment.
	{
		vec3_t right, up;
		AngleVectors(bot->aim, aim_dir, right, up);
		VectorCopy(s->origin, eye);
		eye[2] += s->view_height;
		VectorSubtract(bot->enemy_pos, eye, to_enemy);
	}

	if (to_enemy[0] != 0.0f || to_enemy[1] != 0.0f)
	{
		float yaw_aim, yaw_target;
		yaw_aim = atan2f(aim_dir[1], aim_dir[0]) * 180.0f / (float)M_PI;
		yaw_target = atan2f(to_enemy[1], to_enemy[0]) * 180.0f / (float)M_PI;
		yaw_diff = fabsf(Nano_Wrap180(yaw_target - yaw_aim));
	}
	else
	{
		yaw_diff = 0.0f;
	}

	{
		float target_pitch = -atan2f(to_enemy[2],
										 sqrtf(to_enemy[0] * to_enemy[0] + to_enemy[1] * to_enemy[1]))
										 * 180.0f / (float)M_PI;
		pitch_diff = fabsf(bot->aim[PITCH] - target_pitch);
	}

	if (yaw_diff > tol || pitch_diff > tol * 1.5f)
	{
		if (cvar("k_nano_debug") >= 2)
		{
			G_cprint("[nano] combat not aligned slot=%d dy=%.1f dp=%.1f tol=%.1f\n",
					 s->client, yaw_diff, pitch_diff, tol);
		}
		return false;
	}

	// Line-of-fire trace: don't fire into walls.
	trap_traceline(eye[0], eye[1], eye[2],
				   eye[0] + aim_dir[0] * 4096.0f,
				   eye[1] + aim_dir[1] * 4096.0f,
				   eye[2] + aim_dir[2] * 4096.0f,
				   false, s->client);
	if (g_globalvars.trace_fraction >= 1.0f)
	{
		if (cvar("k_nano_debug") >= 2)
		{
			G_cprint("[nano] combat fire slot=%d enemy=%d\n", s->client, bot->enemy_ent);
		}
		return true;
	}
	if (PROG_TO_EDICT(g_globalvars.trace_ent) == enemy)
	{
		if (cvar("k_nano_debug") >= 2)
		{
			G_cprint("[nano] combat fire slot=%d enemy=%d\n", s->client, bot->enemy_ent);
		}
		return true;
	}
	if (cvar("k_nano_debug") >= 2)
	{
		G_cprint("[nano] combat lof blocked slot=%d\n", s->client);
	}
	return false;
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
						 const vec3_t move_world, int buttons, int impulse)
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
				   forwardmove, sidemove, 0, buttons, impulse);

	if (cvar("k_nano_debug"))
	{
		G_cprint("[nano] cmd slot=%d yaw=%.1f fwd=%d side=%d buttons=%d impulse=%d\n",
				 s->client, bot->aim[1], forwardmove, sidemove, buttons, impulse);
	}
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

	Nano_CombatUpdate(bot, &s);

	Nano_Steer(bot, &s, g, look, move_world, &buttons);
	if (stuck_jump)
	{
		buttons |= NANO_BUTTON_JUMP;
	}

	{
		vec3_t combat_look;
		int impulse = 0;
		qbool combat_active;

		Nano_CombatAim(bot, &s, combat_look);
		combat_active = Nano_CombatActive(bot, &s);
		if (combat_active)
		{
			VectorCopy(combat_look, look);
		}

		Nano_AimSpring(bot, &s, look);

		impulse = Nano_CombatWeapon(bot, &s);
		if (Nano_CombatShouldFire(bot, &s))
		{
			buttons |= NANO_BUTTON_ATTACK;
		}

		Nano_EmitCmd(bot, &s, move_world, buttons, impulse);
	}

	return true;
}

#endif // NANO_SUPPORT
