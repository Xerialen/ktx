/*
 nano_brain.h -- nano-bots: S2a movement brain internal types and pure helpers.

 Per-bot state lives in a static array in nano_brain.c, indexed by edict number.
 Nothing in this header changes shared structs, so NANO_SUPPORT=OFF builds are
 unaffected.
*/
#ifndef KTX_NANO_BRAIN_H
#define KTX_NANO_BRAIN_H

#include "nano.h"

// NOTE: this header expects g_local.h to have been included first (KTX
// convention), so it does not include it itself.

#define NANO_MAX_ROUTE 512
#define NANO_MAX_SLOTS 64       // safe fixed cap; accesses are bounds-checked

#define NANO_ARRIVE_RADIUS      24.0f
#define NANO_REPATH_INTERVAL    0.5f
#define NANO_GOAL_SELECT_INTERVAL 1.5f
#define NANO_GOAL_GIVEUP_TIME   10.0f
#define NANO_MOVE_SPEED         800.0f  // matches rtx BOT_MOVE_SPEED
#define NANO_STUCK_MOVE         16.0f
#define NANO_STUCK_TIME         0.7f
#define NANO_PROGRESS_EPS       32.0f
#define NANO_PROGRESS_TIME      2.5f
#define NANO_AIR_COMMIT_GRACE   0.2f
#define NANO_AIR_COMMIT_MAX     2.5f

#define NANO_BUTTON_ATTACK      1
#define NANO_BUTTON_JUMP        2

// Combat tuning constants (S3a minimum fight loop).
#define NANO_EYE_HEIGHT         22.0f
#define NANO_FOV_BASE           90.0f
#define NANO_REACTION_BASE      0.4f
#define NANO_MEMORY_TIME        5.0f
#define NANO_ROCKET_SPEED       1000.0f
#define NANO_ROCKET_RANGE       800.0f
#define NANO_LG_RANGE           600.0f
#define NANO_SSG_RANGE          500.0f
#define NANO_SG_RANGE           3000.0f
#define NANO_FIRE_TOL_BASE      16.0f
#define NANO_WEAPON_SWITCH_TIME 0.5f

// Snapshot of the bot's edict/engine state for one frame (port of rtx Sense).
typedef struct
{
	float now;
	float frametime;
	int msec;
	vec3_t origin;
	vec3_t v_angle;
	int client;
	int weapon;
	qbool on_ground;
	qbool alive;
	float vz;
	float speed;        // horizontal speed
	qbool has_rl;
	float ammo_rockets;
	float health;
	float armortype;
	float armorvalue;
	qbool quad;

	// S3a combat additions.
	int team;           // numeric team token from getteam()
	int items;
	int ammo_shells;
	int ammo_nails;
	int ammo_cells;
	qbool has_lg;
	qbool has_ssg;
	qbool has_sng;
	qbool has_gl;
	float view_height;  // self->s.v.view_ofs[2]
} nano_sense_t;

// Mutable per-bot brain state (nano-side static array, never in gedict_t).
typedef struct
{
	int route[NANO_MAX_ROUTE];
	int route_len;
	int route_pos;
	int goal_cell;
	int goal_ent;       // edict number, for hysteresis
	float goal_since;
	float repath_time;
	float goal_select_time;
	int air_leg;        // latched jump-gap/double-jump link, -1 if none
	float air_started;
	vec3_t stuck_origin;
	float stuck_since;
	float progress_best;
	float progress_since;
	vec3_t aim;         // smoothed view angles
	vec3_t aim_vel;     // angular velocity
	qbool initialized;

	// S3a combat state.
	int enemy_ent;              // current target edict number, -1 if none
	vec3_t enemy_pos;           // last known / predicted enemy position
	float enemy_seen_time;      // when enemy was last visible
	qbool enemy_visible;        // currently in LOS
	float enemy_visible_since;  // continuous LOS start
	float enemy_reacted_time;   // when reaction delay was satisfied
	int desired_weapon;         // impulse value we want selected
	float weapon_switch_time;   // cooldown for impulse spam
} nano_bot_t;

// Pure helper: wrap an angle to (-180, 180].
static inline float Nano_Wrap180(float a)
{
	while (a > 180.0f)
	{
		a -= 360.0f;
	}
	while (a <= -180.0f)
	{
		a += 360.0f;
	}
	return a;
}

// Pure helper: one critically-damped aim-spring step.
// omega = 6 + k_nano_skill*2 (rtx aim_omega). dt should already be clamped.
static inline void Nano_AimSpringStep(nano_bot_t *bot, const vec3_t look, float omega, float dt)
{
	float d_yaw, d_pitch;

	if (dt < 0.001f)
	{
		dt = 0.001f;
	}
	else if (dt > 0.05f)
	{
		dt = 0.05f;
	}

	// yaw spring with wrap180
	d_yaw = Nano_Wrap180(look[1] - bot->aim[1]);
	bot->aim_vel[1] += (omega * omega * d_yaw - 2.0f * omega * bot->aim_vel[1]) * dt;
	bot->aim[1] = Nano_Wrap180(bot->aim[1] + bot->aim_vel[1] * dt);

	// pitch spring
	d_pitch = look[0] - bot->aim[0];
	bot->aim_vel[0] += (omega * omega * d_pitch - 2.0f * omega * bot->aim_vel[0]) * dt;
	bot->aim[0] = bot->aim[0] + bot->aim_vel[0] * dt;
	if (bot->aim[0] > 89.0f)
	{
		bot->aim[0] = 89.0f;
	}
	else if (bot->aim[0] < -89.0f)
	{
		bot->aim[0] = -89.0f;
	}

	bot->aim[2] = 0.0f;
}

// Pure helpers exposed for unit tests (S3a combat).
static inline qbool Nano_InFOV(const vec3_t aim_angles, const vec3_t origin,
							   const vec3_t target, float fov)
{
	vec3_t dir;
	float yaw, pitch, yaw_to, pitch_to, dy, dp;

	if (fov >= 360.0f)
	{
		return true;
	}

	VectorSubtract(target, origin, dir);
	if (dir[0] == 0.0f && dir[1] == 0.0f && dir[2] == 0.0f)
	{
		return true;
	}

	yaw = aim_angles[YAW];
	pitch = aim_angles[PITCH];

	yaw_to = atan2f(dir[1], dir[0]) * 180.0f / (float)M_PI;
	dy = fabsf(Nano_Wrap180(yaw_to - yaw));

	// Vertical FOV is generous: we only care about horizontal cone for lock-on.
	// Pitch follows the Quake convention (positive = look down), so negate atan2(z, xy).
	pitch_to = -atan2f(dir[2], sqrtf(dir[0] * dir[0] + dir[1] * dir[1])) * 180.0f / (float)M_PI;
	dp = fabsf(pitch_to - pitch);

	return dy <= fov * 0.5f && dp <= fov * 0.5f;
}

static inline void Nano_LeadAim(const vec3_t eye, const vec3_t target, const vec3_t vel,
								float proj_speed, vec3_t out_aim)
{
	vec3_t to, vel_tmp;
	float dist, t;

	VectorSubtract(target, eye, to);
	dist = VectorLength(to);
	if (proj_speed <= 0.0f || dist <= 0.0f)
	{
		VectorCopy(to, out_aim);
		return;
	}

	// Cap lead time so low-skill / close shots don't overshoot wildly.
	t = dist / proj_speed;
	if (t > 1.0f)
	{
		t = 1.0f;
	}

	VectorCopy(target, out_aim);
	VectorCopy(vel, vel_tmp);
	VectorMA(out_aim, t, vel_tmp, out_aim);
	VectorSubtract(out_aim, eye, out_aim);
}

static inline int Nano_WeaponForRange(float dist, int items, int ammo_shells, int ammo_nails,
									  int ammo_rockets, int ammo_cells)
{
	qbool has_ssg = (items & IT_SUPER_SHOTGUN) != 0;
	qbool has_sg = (items & IT_SHOTGUN) != 0;
	qbool has_lg = (items & IT_LIGHTNING) != 0;
	qbool has_rl = (items & IT_ROCKET_LAUNCHER) != 0;
	qbool has_sng = (items & IT_SUPER_NAILGUN) != 0;
	qbool has_ng = (items & IT_NAILGUN) != 0;

	if (dist <= NANO_SSG_RANGE && has_ssg && ammo_shells >= 2)
	{
		return 3;
	}
	if (dist <= NANO_LG_RANGE && has_lg && ammo_cells >= 1)
	{
		return 8;
	}
	if (dist <= NANO_ROCKET_RANGE && has_rl && ammo_rockets >= 1)
	{
		return 7;
	}
	if (has_ssg && ammo_shells >= 2)
	{
		return 3;
	}
	if (has_lg && ammo_cells >= 1)
	{
		return 8;
	}
	if (has_sng && ammo_nails >= 1)
	{
		return 5;
	}
	if (has_sg && ammo_shells >= 1)
	{
		return 2;
	}
	if (has_ng && ammo_nails >= 1)
	{
		return 4;
	}
	return 0;
}

// Main per-frame brain entry point (lives in nano_brain.c).
qbool Nano_BrainFrame(gedict_t *self);

// Pure helpers exposed for unit tests.
qbool Nano_InFOV(const vec3_t aim_angles, const vec3_t origin, const vec3_t target, float fov);
void Nano_LeadAim(const vec3_t eye, const vec3_t target, const vec3_t vel, float proj_speed,
				  vec3_t out_aim);
int Nano_WeaponForRange(float dist, int items, int ammo_shells, int ammo_nails, int ammo_rockets,
						int ammo_cells);

// Reset a slot's brain state. Called when a bot is unmarked or before a new
// bot is marked, so reused edicts never inherit stale goal/route/aim state.
void Nano_BrainClearSlot(int ent);

#endif // KTX_NANO_BRAIN_H
