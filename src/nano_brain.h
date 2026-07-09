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

// Main per-frame brain entry point (lives in nano_brain.c).
qbool Nano_BrainFrame(gedict_t *self);

// Reset a slot's brain state. Called when a bot is unmarked or before a new
// bot is marked, so reused edicts never inherit stale goal/route/aim state.
void Nano_BrainClearSlot(int ent);

#endif // KTX_NANO_BRAIN_H
