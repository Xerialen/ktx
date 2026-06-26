#ifdef BOT_SUPPORT

#include "g_local.h"

#define ARROW_TIME_INCREASE       0.15  // Seconds to advance after NewVelocityForArrow
#define MIN_DEAD_TIME 0.2f
#define MAX_DEAD_TIME 1.0f
#define MOVEPROBE_QWD_MAX_POINTS 96
#define MOVEPROBE_SCHEDULE_NONE -99999.0f

static int moveprobe_transition_active[MAX_CLIENTS];
static int moveprobe_transition_on_ground[MAX_CLIENTS];
static int moveprobe_transition_has_ground_time[MAX_CLIENTS];
static int moveprobe_transition_has_air_time[MAX_CLIENTS];
static int moveprobe_transition_has_seen_time[MAX_CLIENTS];
static float moveprobe_transition_last_ground_time[MAX_CLIENTS];
static float moveprobe_transition_last_air_time[MAX_CLIENTS];
static float moveprobe_transition_last_seen_time[MAX_CLIENTS];
static float moveprobe_transition_since_ground[MAX_CLIENTS];
static float moveprobe_transition_since_air[MAX_CLIENTS];
static float moveprobe_transition_scale_used[MAX_CLIENTS];
static int moveprobe_qwd_active[MAX_CLIENTS];
static int moveprobe_qwd_complete[MAX_CLIENTS];
static int moveprobe_qwd_point_index[MAX_CLIENTS];
static int moveprobe_qwd_point_count[MAX_CLIENTS];
static int moveprobe_qwd_advanced_points[MAX_CLIENTS];
static int moveprobe_qwd_advanced_this_frame[MAX_CLIENTS];
static int moveprobe_qwd_has_seen_time[MAX_CLIENTS];
static int moveprobe_qwd_has_active_since[MAX_CLIENTS];
static float moveprobe_qwd_last_seen_time[MAX_CLIENTS];
static float moveprobe_qwd_active_since[MAX_CLIENTS];
static float moveprobe_qwd_active_seconds[MAX_CLIENTS];
static float moveprobe_qwd_distance[MAX_CLIENTS];
static int moveprobe_s27_handoff_latched[MAX_CLIENTS];

// Mode 10: open-loop replay of an exact human POV command stream. The stream is
// too large for a cvar, so it is read from a file (bots/replay/<name>.cmds); the
// filename rides the small cvar k_fb_moveprobe_replay_file. See komodobots
// scripts/build_replay_command_file.py for the file format (komodobots.replay.v1).
#define MOVEPROBE_REPLAY_MAX_FRAMES 3000

typedef struct
{
	int    msec;
	vec3_t origin;        // human position at this frame; frame 0 is the snap state
	vec3_t velocity;      // human velocity (qu/s); frame 0 is the snap velocity
	vec3_t angles;        // human view angles pitch,yaw,roll
	int    forwardmove;
	int    sidemove;
	int    upmove;
	int    buttons;
	float  cumulative_ms; // sum of msec through this frame (replay timeline)
} moveprobe_replay_frame_t;

// LD-F1 (#95): replay files load per bot slot. A small bounded cache of replay
// stores lets two bots run two different route .cmds files on the same map at
// the same time. A store is reclaimed only when no client slot currently
// resolves to it, so the legacy single-file workflow keeps working unchanged.
#define MOVEPROBE_REPLAY_MAX_FILES 4

typedef struct
{
	char  file[128];     // cvar value this store was loaded from
	int   load_state;    // 0 unused, 1 loaded, -1 failed
	int   count;         // frames currently loaded
	moveprobe_replay_frame_t frames[MOVEPROBE_REPLAY_MAX_FRAMES];
} moveprobe_replay_store_t;

static moveprobe_replay_store_t moveprobe_replay_stores[MOVEPROBE_REPLAY_MAX_FILES];
static int moveprobe_replay_store_for_slot[MAX_CLIENTS]; // store index + 1; 0 = none
static int   moveprobe_replay_active[MAX_CLIENTS];
static int   moveprobe_replay_complete[MAX_CLIENTS];
static int   moveprobe_replay_cursor[MAX_CLIENTS];
static int   moveprobe_replay_has_start[MAX_CLIENTS];
static float moveprobe_replay_start_time[MAX_CLIENTS];
static int   moveprobe_replay_has_seen_time[MAX_CLIENTS];
static float moveprobe_replay_last_seen_time[MAX_CLIENTS];
static vec3_t moveprobe_replay_expected[MAX_CLIENTS];
static float moveprobe_replay_divergence[MAX_CLIENTS];
static float moveprobe_replay_divergence_h[MAX_CLIENTS];
static float moveprobe_replay_divergence_v[MAX_CLIENTS];
static float moveprobe_replay_corr_accum[MAX_CLIENTS]; // mode 12: cumulative yaw correction (deg)
static float moveprobe_replay_corr_max[MAX_CLIENTS];   // mode 12: max per-frame yaw correction (deg)
static int   moveprobe_replay_cmd_msec[MAX_CLIENTS];   // optional recorded-msec trap_SetBotCMD override
static int   moveprobe_replay_step_emitted[MAX_CLIENTS]; // step-cursor mode: first command already emitted?
static int   moveprobe_replay_start_resnaps[MAX_CLIENTS]; // optional early replay-start divergence repair count
static int   moveprobe_replay_attempt_num[MAX_CLIENTS];    // loop: attempt counter (1-based)
static float moveprobe_replay_attempt_maxh[MAX_CLIENTS];   // loop: max divH this attempt (qu)
static int   moveprobe_replay_attempt_break[MAX_CLIENTS];  // loop: first cursor divH>corridor (-1 none)
static float moveprobe_replay_attempt_finalh[MAX_CLIENTS]; // loop: max divH in final segment (qu)
static float moveprobe_replay_loop_cooldown[MAX_CLIENTS];  // loop: do not re-arm before this time
static int   moveprobe_accel_jump_press[MAX_CLIENTS];      // mode 13: last-frame jump press (toggle)
static int   moveprobe_accel_strafe_sign[MAX_CLIENTS];     // mode 13: current strafe rotation sign (+1/-1)
static float moveprobe_s23_deleg_since[MAX_CLIENTS];       // mode 23: when continuous climb-delegation began
static gedict_t *moveprobe_s23_deleg_marker[MAX_CLIENTS];  // mode 23: marker the delegation is climbing toward
static gedict_t *moveprobe_s23_carrot_done[MAX_CLIENTS];   // mode 23: marker already early-handed-over (edge trigger)
static gedict_t *moveprobe_s23_prec_marker[MAX_CLIENTS];   // mode 23: precision-governor target marker (sharp-corner leg)
static float moveprobe_s23_prec_since[MAX_CLIENTS];        // mode 23: when precision mode engaged (timeout escape)
static int moveprobe_s23_launch_done[MAX_CLIENTS];         // mode 23: circle-jump launch one-shot latch (A3 #75)
static float moveprobe_s23_launch_since[MAX_CLIENTS];      // mode 23: launch first-eval time (<=0 = not evaluated yet)
static int moveprobe_s23_zjump_phase[MAX_CLIENTS];         // mode 23: ztricks terminal carve phase (0 off, 1 tracking, 2 released)
static int moveprobe_s23_zjump_armed[MAX_CLIENTS];         // mode 23: ztricks speed/distance release window is active
static int moveprobe_s23_zjump_release_rule[MAX_CLIENTS];  // mode 23: 0 none, 1 formula, 2 late-lip backstop
static float moveprobe_s23_zjump_d_lip[MAX_CLIENTS];       // mode 23: horizontal distance to configured lip x
static float moveprobe_s23_zjump_vh[MAX_CLIENTS];          // mode 23: horizontal speed at terminal-carve eval
static float moveprobe_s23_zjump_vel_yaw[MAX_CLIENTS];     // mode 23: velocity yaw at terminal-carve eval
static float moveprobe_s23_zjump_target_yaw[MAX_CLIENTS];  // mode 23: yaw from bot to configured landing target
static float moveprobe_s23_zjump_target_err[MAX_CLIENTS];  // mode 23: target_yaw - vel_yaw, wrapped
static float moveprobe_s23_zjump_yaw_lead[MAX_CLIENTS];    // mode 23: view yaw - vel_yaw, wrapped
static float moveprobe_orbit_yaw[MAX_CLIENTS];             // mode 14: base heading yaw (the "direction"/orbit)
static int   moveprobe_orbit_init[MAX_CLIENTS];            // mode 14: base heading seeded?
static int   moveprobe_orbit_jump[MAX_CLIENTS];            // mode 14: last-frame jump press (toggle)
static int   moveprobe_orbit_sign[MAX_CLIENTS];            // mode 14: current carve sign (+1/-1)
static int   moveprobe_cstrafe_init[MAX_CLIENTS];          // mode 15: coupled circle-strafe seeded?
static int   moveprobe_cstrafe_side[MAX_CLIENTS];          // mode 15: current strafe sign (+1=right/-1=left)
static float moveprobe_cstrafe_view[MAX_CLIENTS];          // mode 15: integrated view yaw (deg)
static int   moveprobe_cstrafe_jump[MAX_CLIENTS];          // mode 15: last-frame jump press (toggle)
static float moveprobe_cstrafe_flip_t[MAX_CLIENTS];        // mode 15: server time of last (side,turn) flip
static int   moveprobe_curl_sign[MAX_CLIENTS];             // mode 16: curl direction (+1 CCW / -1 CW)
static int   moveprobe_curl_init[MAX_CLIENTS];             // mode 16: seeded?
static int   moveprobe_curl_jump[MAX_CLIENTS];             // mode 16: last-frame jump press (toggle)
static int   moveprobe_curl_dirset[MAX_CLIENTS];           // mode 16: curl direction chosen from geometry?
static int   moveprobe_fig8_target[MAX_CLIENTS];           // mode 17: target curl sign (+1/-1)
static float moveprobe_fig8_ramp[MAX_CLIENTS];             // mode 17: effective curl, ramps -1..+1 (smooth flip)
static int   moveprobe_fig8_init[MAX_CLIENTS];             // mode 17: seeded?

static float BotMoveProbeWrap180(float angle)
{
	while (angle > 180.0f) angle -= 360.0f;
	while (angle < -180.0f) angle += 360.0f;
	return angle;
}

static float BotMoveProbeQuadratic(float x,
								   float x0, float y0,
								   float x1, float y1,
								   float x2, float y2)
{
	float d0 = (x0 - x1) * (x0 - x2);
	float d1 = (x1 - x0) * (x1 - x2);
	float d2 = (x2 - x0) * (x2 - x1);

	if ((d0 == 0.0f) || (d1 == 0.0f) || (d2 == 0.0f))
	{
		return y1;
	}
	return y0 * ((x - x1) * (x - x2) / d0)
		+ y1 * ((x - x0) * (x - x2) / d1)
		+ y2 * ((x - x0) * (x - x1) / d2);
}

static void BotMoveProbeZtricksReferenceCurve(float d_lip,
											  float *vel_yaw,
											  float *view_yaw)
{
	float d = bound(0.0f, d_lip, 91.4f);

	// Human attempt 11 terminal sweep, modeled in d_lip space with local
	// quadratic segments. Angles are already unwrapped around the jump.
	if (d > 25.0f)
	{
		*vel_yaw = BotMoveProbeQuadratic(d, 91.4f, 41.4f, 71.9f, 27.5f,
										 25.0f, -3.3f);
		*view_yaw = BotMoveProbeQuadratic(d, 91.4f, 39.1f, 71.9f, 23.8f,
										  25.0f, -11.4f);
	}
	else
	{
		*vel_yaw = BotMoveProbeQuadratic(d, 25.0f, -3.3f, 12.8f, -11.3f,
										 0.0f, -11.3f);
		*view_yaw = BotMoveProbeQuadratic(d, 25.0f, -11.4f, 12.8f, -19.0f,
										  0.0f, -24.6f);
	}
}
static int   moveprobe_fig8_jump[MAX_CLIENTS];             // mode 17: last-frame jump press (toggle)
static float moveprobe_fig8_flip_t[MAX_CLIENTS];           // mode 17: server time of last lobe-switch
static float moveprobe_orbit18_yaw[MAX_CLIENTS];           // mode 18: orbit view yaw (rotates at OMEGA deg/s)
static int   moveprobe_orbit18_jump[MAX_CLIENTS];          // mode 18: last-frame jump press (bhop toggle)
static int   moveprobe_s19_wallcd[MAX_CLIENTS];            // mode 19: wall-flip debounce cooldown (frames)
static int   moveprobe_s19_flipframes[MAX_CLIENTS];        // mode 19: air frames since last serpentine cadence flip
static int   moveprobe_s21_wp[MAX_CLIENTS];                // mode 21: current path-waypoint index (proximity-advanced)
static float moveprobe_s21_prevorg[MAX_CLIENTS][3];        // mode 21: bot origin last frame (teleport detection)
static int   moveprobe_s21_has_prev[MAX_CLIENTS];          // mode 21: prevorg valid?
static int   moveprobe_s21_teleported[MAX_CLIENTS];        // mode 21: passed the route teleporter yet?
static int   moveprobe_s21_descend_latch[MAX_CLIENTS];     // mode 21: committed to the ballistic pit-descent into the RL nook (sticky)
static float moveprobe_s22_sim_yaw[MAX_CLIENTS];           // mode 22: integrated (simulated) view yaw, deg -- the rate-limited "mouse"
static int   moveprobe_s22_strafe_sign[MAX_CLIENTS];       // mode 22: held strafe sign (+1=right/-1=left)
static int   moveprobe_s22_init[MAX_CLIENTS];              // mode 22: sim_yaw seeded?
static float moveprobe_s22_flip_t[MAX_CLIENTS];            // mode 22: server time of last strafe-sign flip
static int   moveprobe_s25_active[MAX_CLIENTS];            // mode 25: frame was evaluated by human-mouse catch-up
static int   moveprobe_s25_engaged[MAX_CLIENTS];           // mode 25: catch-up replaced the recorded movement vector
static int   moveprobe_s25_reason[MAX_CLIENTS];            // mode 25: bitmask 1 speed-gap, 2 path-div, 4 velsign, 8 path-blend, 16 phase-recover, 32 phase-human-cmd, 64 phase2-move, 128 phase-jump, 256 phase-gap-boost, 512 phase-yaw-offset, 1024 phase-human-scale, 2048 phase-lane-nudge
static int   moveprobe_s25_sign[MAX_CLIENTS];              // mode 25: chosen velocity-relative strafe sign
static float moveprobe_s25_hs[MAX_CLIENTS];                // mode 25: live horizontal speed
static float moveprobe_s25_target_hs[MAX_CLIENTS];         // mode 25: human-frame horizontal speed
static float moveprobe_s25_speed_gap[MAX_CLIENTS];         // mode 25: human speed minus live speed
static float moveprobe_s25_rotation[MAX_CLIENTS];          // mode 25: velocity-relative wishdir rotation
static float moveprobe_s25_wish_yaw[MAX_CLIENTS];          // mode 25: final world-space wishdir yaw
static float moveprobe_s25_vel_yaw[MAX_CLIENTS];           // mode 25: live velocity yaw
static float moveprobe_s25_target_vel_yaw[MAX_CLIENTS];    // mode 25: human velocity yaw
static float moveprobe_s25_target_vel_err[MAX_CLIENTS];    // mode 25: human velocity yaw minus live velocity yaw
static float moveprobe_s25_out_fwd[MAX_CLIENTS];           // mode 25: projected forward command
static float moveprobe_s25_out_side[MAX_CLIENTS];          // mode 25: projected side command

// ---------------------------------------------------------------------------
// LD-F1 (#95): per-slot moveprobe cvar convention.
//
// k_fb_moveprobe_<param>_s<N> overrides k_fb_moveprobe_<param> for one bot,
// where N is the bot's edict/client number -- the same `ed` printed in
// FBMOVEPROBE_CMD rows, so telemetry rows join to assignments directly.
// An unset/empty per-slot cvar falls back to the global cvar, which keeps
// existing single-route configs byte-identical (additive guarantee).
// Malformed per-slot values fail LOUDLY: an FBMOVEPROBE_PERSLOT_ERROR console
// row plus the bot held at spawn (zeroed movement), never a silent fallback.
// Per-slot wiring exists for: mode, replay_file, fixed_goal, spawn_origin,
// spawn_velocity.
// ---------------------------------------------------------------------------

// Set by UpdateGoal() (bot_botgoals.c) when a per-slot fixed_goal value is
// malformed or names a marker absent on this map; BotApplyMoveProbe() holds
// the bot while it is set.
int fb_moveprobe_perslot_goal_error[MAX_CLIENTS];

static float moveprobe_perslot_error_logged[MAX_CLIENTS]; // throttle: 1 row / 2 s

// Reads param for this bot: per-slot cvar first, global cvar as fallback.
// Returns 1 when the per-slot cvar supplied the value, 0 on global fallback.
int BotMoveProbeCvarStringForBot(gedict_t *self, const char *param, char *out, int out_size)
{
	char name[96];

	snprintf(name, sizeof(name), "k_fb_moveprobe_%s_s%d", param, NUM_FOR_EDICT(self));
	trap_cvar_string(name, out, out_size);
	if (out[0])
	{
		return 1;
	}
	snprintf(name, sizeof(name), "k_fb_moveprobe_%s", param);
	trap_cvar_string(name, out, out_size);
	return 0;
}

// Loud-failure row for a bad per-slot value (lab precedent #77). Printed
// unconditionally (not gated on k_fb_moveprobe_log_commands) and throttled
// per slot so a held bot does not flood screen.log at command rate.
void BotMoveProbeReportPerSlotError(gedict_t *self, const char *param, const char *value,
									const char *reason)
{
	char shown[64];
	int slot = NUM_FOR_EDICT(self) - 1;
	int i;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}
	if (moveprobe_perslot_error_logged[slot]
		&& (g_globalvars.time >= moveprobe_perslot_error_logged[slot])
		&& ((g_globalvars.time - moveprobe_perslot_error_logged[slot]) < 2.0f))
	{
		return;
	}
	moveprobe_perslot_error_logged[slot] = g_globalvars.time;

	// Keep the row single-token-per-field parseable: spaces become commas.
	strlcpy(shown, (value && value[0]) ? value : "(empty)", sizeof(shown));
	for (i = 0; shown[i]; i++)
	{
		if (shown[i] == ' ' || shown[i] == '\t')
		{
			shown[i] = ',';
		}
	}

	G_cprint("FBMOVEPROBE_PERSLOT_ERROR time=%.3f ed=%d name=%s param=%s value=%s reason=%s\n",
			 g_globalvars.time, NUM_FOR_EDICT(self), self->netname, param, shown, reason);
}

// Strict integer scan: optional sign + decimal digits only.
static int BotMoveProbeParseStrictInt(const char *text, int *value)
{
	const char *cursor = text;
	int sign = 1;
	long accum = 0;
	int digits = 0;

	if (*cursor == '-' || *cursor == '+')
	{
		sign = (*cursor == '-') ? -1 : 1;
		cursor++;
	}
	for (; *cursor; cursor++)
	{
		if (*cursor < '0' || *cursor > '9')
		{
			return 0;
		}
		accum = (accum * 10) + (*cursor - '0');
		if (accum > 0x7fffffffL)
		{
			return 0;
		}
		digits++;
	}
	if (!digits)
	{
		return 0;
	}
	*value = (int)(sign * accum);
	return 1;
}

// Resolves an integer moveprobe cvar with the per-slot convention. Returns 0
// on success (writes *value and *from_slot), -1 when the per-slot value
// exists but is not an integer (the error row is printed here; the caller
// must hold the bot). The global fallback keeps stock cvar() coercion so
// behavior with no per-slot cvars set is unchanged.
int BotMoveProbeCvarIntForBot(gedict_t *self, const char *param, int *value, int *from_slot)
{
	char buf[64];

	*from_slot = BotMoveProbeCvarStringForBot(self, param, buf, sizeof(buf));
	if (!*from_slot)
	{
		char name[96];

		snprintf(name, sizeof(name), "k_fb_moveprobe_%s", param);
		*value = (int)cvar(name);
		return 0;
	}
	if (!BotMoveProbeParseStrictInt(buf, value))
	{
		BotMoveProbeReportPerSlotError(self, param, buf, "not_an_integer");
		return -1;
	}
	return 0;
}

//#define DEBUG_MOVEMENT

void SetLinkedMarker(gedict_t *player, gedict_t *marker, char *explanation)
{
	gedict_t *touch = player->fb.touch_marker;

	if ((marker != player->fb.linked_marker) && FrogbotOptionEnabled(FB_OPTION_SHOW_MOVEMENT_LOGIC))
	{
		G_sprint(player, 2, "linked to %3d/%s, touching %3d/%s g %s (%s)\n",
					marker ? marker->fb.index + 1 : -1, marker ? marker->classname : "(null)",
					touch ? touch->fb.index + 1 : -1, touch ? touch->classname : "(null)",
					g_edicts[player->s.v.goalentity].classname, explanation ? explanation : "");
	}

	if (player->fb.debug_path)
	{
		G_bprint(2, "%3.2f: linked to %3d/%s, touching %3d/%s g %s (%s)\n", g_globalvars.time,
					marker ? marker->fb.index + 1 : -1, marker ? marker->classname : "(null)",
					touch ? touch->fb.index + 1 : -1, touch ? touch->classname : "(null)",
					g_edicts[player->s.v.goalentity].classname, explanation ? explanation : "");
	}

	player->fb.linked_marker = marker;
}

void SetJumpFlag(gedict_t *player, qbool jumping, const char *explanation)
{
	if (jumping != player->fb.jumping)
	{
		if (self->fb.debug_path)
		{
			G_bprint(PRINT_HIGH, "%3.2f: jumping (%s)\n", g_globalvars.time, explanation);
		}

		if (FrogbotOptionEnabled(FB_OPTION_SHOW_MOVEMENT_LOGIC))
		{
			G_sprint(player, PRINT_HIGH, "%3.2f: jumping (%s)\n", g_globalvars.time, explanation);
		}
	}

	player->fb.jumping = jumping;
}

void SetDirectionMove(gedict_t *self, vec3_t dir_move, const char *explanation)
{
	VectorCopy(dir_move, self->fb.dir_move_);
	self->fb.dir_speed = VectorNormalize(self->fb.dir_move_);

	/*	if (self->fb.debug_path) {
	 G_bprint (PRINT_HIGH, "%3.2f: SetDirection(%4d %4d %4d): %s\n", g_globalvars.time, PASSSCALEDINTVEC3 (self->fb.dir_move_, 320), explanation);
	 }
	 if (FrogbotOptionEnabled (FB_OPTION_SHOW_MOVEMENT_LOGIC)) {
	 G_sprint (self, PRINT_HIGH, "%3.2f: SetDirection(%4d %4d %4d): %s\n", g_globalvars.time, PASSSCALEDINTVEC3 (self->fb.dir_move_, 320), explanation);
	 }*/
}

void NewVelocityForArrow(gedict_t *self, vec3_t dir_move, const char *explanation)
{
	SetDirectionMove(self, dir_move, explanation);
	self->fb.arrow_time = g_globalvars.time + ARROW_TIME_INCREASE;
}

static qbool BotRequestRespawn(gedict_t *self)
{
	float time_dead = min(g_globalvars.time - self->fb.last_death, MAX_DEAD_TIME);

	return ((self->s.v.deadflag == DEAD_RESPAWNABLE) && (time_dead > MIN_DEAD_TIME)
			&& (g_random() < (time_dead / MAX_DEAD_TIME)));
}

static void PM_Accelerate(vec3_t vel_after_friction, qbool onGround, vec3_t orig_wishdir,
							vec3_t vel_result, qbool trace)
{
	float addspeed, accelspeed, currentspeed;
	float wishspeed = 320; // FIXME: assuming attemping sv_maxspeed
	float accel = 10; // FIXME: assumption
	vec3_t velocity;
	vec3_t wishdir;

	VectorCopy(vel_after_friction, velocity);
	if (onGround)
	{
		velocity[2] = 0;
	}

	wishdir[0] = orig_wishdir[0];
	wishdir[1] = orig_wishdir[1];
	wishdir[2] = 0;
	wishspeed = VectorNormalize(wishdir);
	wishspeed = sv_maxspeed; // fixme: we scale back up to maximum just as passing command

	currentspeed = DotProduct(velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
	{
		VectorCopy(vel_after_friction, vel_result);
#ifdef DEBUG_MOVEMENT
		if (trace) {
			G_bprint (PRINT_HIGH, "%3.2f,KTX,%4.1f,%4.1f,%4.1f,%4.1f,%4.1f,%4.1f,%3.2f,%3.2f,%3.2f,0,%4.1f,%4.1f,%4.1f\n",
				g_globalvars.time, PASSVEC3 (vel_after_friction), PASSVEC3 (wishdir), wishspeed, currentspeed, addspeed, PASSVEC3 (vel_result)
			);
		}
#endif
		return;
	}

	accelspeed = accel * g_globalvars.frametime * wishspeed;
	VectorMA(vel_after_friction, min(accelspeed, addspeed), wishdir, vel_result);
#ifdef DEBUG_MOVEMENT
	if (trace) {
		G_bprint (PRINT_HIGH, "%3.2f,KTX,%4.1f,%4.1f,%4.1f,%4.1f,%4.1f,%4.1f,%3.2f,%3.2f,%3.2f,%3.2f,%4.1f,%4.1f,%4.1f\n",
			g_globalvars.time, PASSVEC3 (vel_after_friction), PASSVEC3 (wishdir), wishspeed, currentspeed, addspeed, accelspeed, PASSVEC3 (vel_result)
		);
	}
#endif
}

static void ApplyPhysics(gedict_t *self)
{
	float drop = 0;
	vec3_t expected_velocity;
	float vel_length = 0;
	float hor_speed_squared;
	float movement_skill = bound(0, self->fb.skill.movement, 1.0);
	qbool onGround = (int)self->s.v.flags & FL_ONGROUND;

	// Just perform the move if we're backing away
	if (FUTURE(arrow_time2))
	{
		return;
	}

	if ((deathmatch >= 4) && isDuel() && !self->fb.skill.wiggle_run_dmm4)
	{
		return;
	}

	// Step 1: Apply friction
	VectorCopy(self->s.v.velocity, expected_velocity);
	vel_length = VectorLength(expected_velocity);
	if (vel_length < 1)
	{
		return;
	}

	if (self->s.v.waterlevel >= 2)
	{
		// Swimming...
		float waterfriction = cvar("sv_waterfriction");

		drop = vel_length * waterfriction * self->s.v.waterlevel * g_globalvars.frametime;
	}
	else if (onGround)
	{
		// FIXME: friction is doubled if player is about to drop off a ledge
		float stopspeed = cvar("sv_stopspeed");
		float friction = cvar("sv_friction");
		float control = vel_length < stopspeed ? stopspeed : vel_length;

		drop = control * friction * g_globalvars.frametime;
	}

	if (drop)
	{
		float new_vel = max(vel_length - drop, 0);

		VectorScale(expected_velocity, new_vel / vel_length, expected_velocity);

		vel_length = new_vel;
	}
	else
	{
		vel_length = VectorLength(expected_velocity);
	}

	// Step 2: change direction to maximise acceleration in desired direction
	if (self->s.v.waterlevel >= 2)
	{
		// Water movement
	}
	else
	{
		float min_numerator = onGround ? 319 : 29;
		float max_numerator = onGround ? 281.6 : -8.4;
		float used_numerator;
		float max_incr;
		vec3_t current_direction;
		vec3_t original_direction;

		// Gravity kicks in
		/*
		 if (!onGround)
		 expected_velocity[2] -= self->gravity * cvar ("sv_gravity") * g_globalvars.frametime;
		 else
		 expected_velocity[2] = 0;
		 */

		// Ground & air acceleration is the same
		hor_speed_squared = (expected_velocity[0] * expected_velocity[0]
				+ expected_velocity[1] * expected_velocity[1]);
		if (onGround && hor_speed_squared < sv_maxspeed * sv_maxspeed * 0.8 * 0.8)
		{
			return;
		}

		self->fb.dir_move_[2] = 0;
		normalize(self->fb.dir_move_, original_direction);
		normalize(expected_velocity, current_direction);
		used_numerator = min_numerator + movement_skill * (max_numerator - min_numerator);
		max_incr = used_numerator * used_numerator;
		if (hor_speed_squared >= max_incr)
		{
			vec3_t perpendicular;
			vec3_t up_vector =
				{ 0, 0, 1 };
			float rotation = acos(max_incr / hor_speed_squared) * 180 / M_PI;

			// Find out if rotation should be positive or negative
			CrossProduct(current_direction, original_direction, perpendicular);

			if ((self->fb.path_state & BOTPATH_CURLJUMP_HINT) && !onGround)
			{
				// Once in the air, we rotate in opposite direction
				// FIXME: THIS IS UGLY HACK
				if (framecount % 3)
				{
					rotation = 0;
				}
				else if (self->fb.angle_hint > 0)
				{
					rotation = -rotation;
				}
			}
			else if (deathmatch == 4)
			{
				if (self->fb.wiggle_run_dir == 0)
				{
					self->fb.wiggle_increasing = perpendicular[2] > 0;
					self->fb.wiggle_run_dir = self->fb.wiggle_increasing ? 1 : -1;
				}
				else if ((self->fb.wiggle_run_dir > self->fb.skill.wiggle_run_limit)
						&& (perpendicular[2] < 0))
				{
					self->fb.wiggle_increasing = false;
				}
				else if ((self->fb.wiggle_run_dir < -self->fb.skill.wiggle_run_limit)
						&& (perpendicular[2] > 0))
				{
					self->fb.wiggle_increasing = 1;
				}
				else if (self->fb.wiggle_increasing)
				{
					++self->fb.wiggle_run_dir;
				}
				else
				{
					--self->fb.wiggle_run_dir;
				}

				if (self->fb.wiggle_increasing)
				{
					rotation = -rotation;
				}
			}
			else if (perpendicular[2] < 0)
			{
				rotation = -rotation;
			}

			if (rotation)
			{
				vec3_t proposed_dir;
				vec3_t vel_after_rot;
				vec3_t vel_std;
				float dp_std, dp_rot;

				RotatePointAroundVector(proposed_dir, up_vector, current_direction, rotation);

				// Calculate what mvdsv will do (roughly)
				PM_Accelerate(expected_velocity, (int)self->s.v.flags & FL_ONGROUND, proposed_dir,
								vel_after_rot, false);
				PM_Accelerate(expected_velocity, (int)self->s.v.flags & FL_ONGROUND,
								current_direction, vel_std, false);

				// Only rotate if 'better' than moving normally
				dp_rot = DotProduct(vel_after_rot, original_direction);
				dp_std = DotProduct(vel_std, original_direction);

				if ((dp_rot > dp_std) || (dp_rot >= 0.9))
				{
					VectorCopy(proposed_dir, self->fb.dir_move_);
					if (self->fb.debug_path)
					{
						PM_Accelerate(expected_velocity, (int)self->s.v.flags & FL_ONGROUND,
										proposed_dir, vel_after_rot, true);
					}
				}
				else if (self->fb.debug_path)
				{
					PM_Accelerate(expected_velocity, (int)self->s.v.flags & FL_ONGROUND,
									current_direction, vel_std, true);
				}
			}
			else
			{
#ifdef DEBUG_MOVEMENT
				if (self->fb.debug_path && ! onGround) {
					G_bprint (PRINT_HIGH, "> AirControl rotation: <ignoring>\n");
				}
#endif
			}
		}
	}
}

float AverageTraceAngle(gedict_t *self, qbool debug, qbool report)
{
	vec3_t back_left, projection, incr;
	int angles[] =
		{ 45, 30, 15, 0, -15, -30, -45 };
	int i;
	float best_angle = 0;
	float best_angle_frac = 0;
	float total_angle = 0;
	float avg_angle;

	float distance = 320;

	if (self->fb.path_state & JUMP_LEDGE)
	{
		return 0;
	}

	if (debug)
	{
		trap_makevectors(self->s.v.angles);
	}
	else
	{
		trap_makevectors(self->fb.dir_move_);
	}

	VectorMA(self->s.v.origin, -VEC_HULL_MIN[0], g_globalvars.v_forward, back_left);
	VectorMA(back_left, VEC_HULL_MIN[1], g_globalvars.v_right, back_left);

	VectorScale(g_globalvars.v_right,
				(VEC_HULL_MAX[0] - VEC_HULL_MIN[0]) / (sizeof(angles) / sizeof(angles[0]) - 1),
				incr);

	if (debug)
	{
		G_bprint(2, "Current origin: %d %d %d\n", PASSINTVEC3(self->s.v.origin));
		G_bprint(2, "Current angles: %d %d\n", PASSINTVEC2(self->s.v.angles));
	}

	for (i = 0; i < sizeof(angles) / sizeof(angles[0]); ++i)
	{
		int angle = angles[i];

		RotatePointAroundVector(projection, g_globalvars.v_up, g_globalvars.v_forward, angle);
		VectorMA(back_left, distance, projection, projection);
		traceline(PASSVEC3(back_left), PASSVEC3(projection), false, self);

		if (g_globalvars.trace_fraction == 1)
		{
			total_angle += angle * 1.5; // bonus for success
		}
		else if (g_globalvars.trace_fraction > 0.4)
		{
			total_angle += angle * g_globalvars.trace_fraction;
		}

		if (debug)
		{
			G_bprint(2, "Angle: %d => [%d %d %d] [%d %d %d] = %f\n", angle, PASSINTVEC3(back_left),
						PASSINTVEC3(projection), g_globalvars.trace_fraction);
		}

		if ((i == 0) || g_globalvars.trace_fraction > best_angle_frac)
		{
			best_angle = angle;
			best_angle_frac = g_globalvars.trace_fraction;
		}

		VectorAdd(back_left, incr, back_left);
	}

	avg_angle = total_angle / (sizeof(angles) / sizeof(angles[0]));

	if (debug)
	{
		G_bprint(2, "Best angle: %f\n", best_angle);
		G_bprint(2, "Total angle: %f\n", avg_angle);
	}

	return avg_angle;
}

static void BestJumpingDirection(gedict_t *self)
{
	float raw_avg_angle = AverageTraceAngle(self, false, self->fb.debug_path);
	float avg_angle;

	if (raw_avg_angle < 0)
	{
		avg_angle = min(raw_avg_angle, -15);
	}
	else
	{
		avg_angle = max(raw_avg_angle, 15);
	}

	RotatePointAroundVector(self->fb.dir_move_, g_globalvars.v_up, g_globalvars.v_forward,
							avg_angle);
}

static void BotMoveProbeResetTransitionTiming(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	moveprobe_transition_has_ground_time[slot] = 0;
	moveprobe_transition_has_air_time[slot] = 0;
	moveprobe_transition_has_seen_time[slot] = 0;
	moveprobe_transition_last_ground_time[slot] = 0.0f;
	moveprobe_transition_last_air_time[slot] = 0.0f;
	moveprobe_transition_last_seen_time[slot] = 0.0f;
}

static void BotMoveProbePrepareTransitionTiming(int slot)
{
	float stale_gap = 1.0f;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	// A clock reset or long command gap means this slot probably changed map,
	// mode, or bot session; do not inherit transition timing across that gap.
	if (!moveprobe_transition_has_seen_time[slot]
		|| (g_globalvars.time < moveprobe_transition_last_seen_time[slot])
		|| ((g_globalvars.time - moveprobe_transition_last_seen_time[slot]) > stale_gap))
	{
		BotMoveProbeResetTransitionTiming(slot);
	}

	moveprobe_transition_has_seen_time[slot] = 1;
	moveprobe_transition_last_seen_time[slot] = g_globalvars.time;
}

static qbool BotMoveProbeTransitionActive(gedict_t *self, int slot, qbool jumping, float window)
{
	qbool on_ground = ((int)self->s.v.flags & FL_ONGROUND) != 0;
	float since_ground = 999.0f;
	float since_air = 999.0f;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return false;
	}

	BotMoveProbePrepareTransitionTiming(slot);

	if (on_ground)
	{
		moveprobe_transition_last_ground_time[slot] = g_globalvars.time;
		moveprobe_transition_has_ground_time[slot] = 1;
	}
	else
	{
		moveprobe_transition_last_air_time[slot] = g_globalvars.time;
		moveprobe_transition_has_air_time[slot] = 1;
	}

	if (moveprobe_transition_has_ground_time[slot])
	{
		since_ground = g_globalvars.time - moveprobe_transition_last_ground_time[slot];
	}
	if (moveprobe_transition_has_air_time[slot])
	{
		since_air = g_globalvars.time - moveprobe_transition_last_air_time[slot];
	}

	moveprobe_transition_on_ground[slot] = on_ground;
	moveprobe_transition_since_ground[slot] = since_ground;
	moveprobe_transition_since_air[slot] = since_air;
	moveprobe_transition_active[slot] = (on_ground && jumping)
		|| (!on_ground && (since_ground <= window))
		|| (on_ground && (since_air <= window));

	return moveprobe_transition_active[slot];
}

static void BotMoveProbeResetTransition(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	moveprobe_transition_active[slot] = 0;
	moveprobe_transition_on_ground[slot] = 0;
	moveprobe_transition_since_ground[slot] = 999.0f;
	moveprobe_transition_since_air[slot] = 999.0f;
	moveprobe_transition_scale_used[slot] = 1.0f;
}

static void BotMoveProbeResetQwdSession(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	moveprobe_qwd_active[slot] = 0;
	moveprobe_qwd_complete[slot] = 0;
	moveprobe_qwd_point_index[slot] = 0;
	moveprobe_qwd_point_count[slot] = 0;
	moveprobe_qwd_advanced_points[slot] = 0;
	moveprobe_qwd_advanced_this_frame[slot] = 0;
	moveprobe_qwd_has_seen_time[slot] = 0;
	moveprobe_qwd_has_active_since[slot] = 0;
	moveprobe_qwd_last_seen_time[slot] = 0.0f;
	moveprobe_qwd_active_since[slot] = 0.0f;
	moveprobe_qwd_active_seconds[slot] = 0.0f;
	moveprobe_qwd_distance[slot] = 999999.0f;
	moveprobe_s27_handoff_latched[slot] = 0;
}

static void BotMoveProbePrepareQwdFrame(int slot)
{
	float stale_gap = 1.0f;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	if (!moveprobe_qwd_has_seen_time[slot]
		|| (g_globalvars.time < moveprobe_qwd_last_seen_time[slot])
		|| ((g_globalvars.time - moveprobe_qwd_last_seen_time[slot]) > stale_gap))
	{
		BotMoveProbeResetQwdSession(slot);
	}

	moveprobe_qwd_has_seen_time[slot] = 1;
	moveprobe_qwd_last_seen_time[slot] = g_globalvars.time;
	moveprobe_qwd_advanced_this_frame[slot] = 0;
	if (moveprobe_qwd_has_active_since[slot])
	{
		moveprobe_qwd_active_seconds[slot] = g_globalvars.time - moveprobe_qwd_active_since[slot];
	}
}

static int BotMoveProbeReadQwdWaypointChunk(vec3_t points[MOVEPROBE_QWD_MAX_POINTS], int count, const char *name)
{
	char buffer[1024];
	char *cursor;

	if (count >= MOVEPROBE_QWD_MAX_POINTS)
	{
		return count;
	}

	trap_cvar_string(name, buffer, sizeof(buffer));
	cursor = buffer;
	while (*cursor && count < MOVEPROBE_QWD_MAX_POINTS)
	{
		char *next;

		points[count][0] = atof(cursor);
		next = strchr(cursor, ',');
		if (!next)
		{
			break;
		}
		cursor = next + 1;

		points[count][1] = atof(cursor);
		next = strchr(cursor, ',');
		if (!next)
		{
			break;
		}
		cursor = next + 1;

		points[count][2] = atof(cursor);
		count++;

		next = strchr(cursor, ';');
		if (!next)
		{
			break;
		}
		cursor = next + 1;
	}

	return count;
}

static int BotMoveProbeReadQwdWaypoints(vec3_t points[MOVEPROBE_QWD_MAX_POINTS])
{
	char name[64];
	int chunk;
	int count = 0;

	count = BotMoveProbeReadQwdWaypointChunk(points, count, "k_fb_moveprobe_qwd_waypoints");
	for (chunk = 1; chunk <= 8 && count < MOVEPROBE_QWD_MAX_POINTS; chunk++)
	{
		snprintf(name, sizeof(name), "k_fb_moveprobe_qwd_waypoints_%d", chunk);
		count = BotMoveProbeReadQwdWaypointChunk(points, count, name);
	}

	return count;
}


static char *BotMoveProbeScheduleNext(char *cursor)
{
	char *comma = strchr(cursor, ',');
	char *semi = strchr(cursor, ';');

	if (!comma) return semi;
	if (!semi) return comma;
	return (comma < semi) ? comma : semi;
}

static int BotMoveProbeReadScheduleInt(const char *name, int index, int fallback)
{
	char buffer[2048];
	char *cursor;
	int current = 0;
	int value = fallback;

	if (index < 0)
	{
		return fallback;
	}

	trap_cvar_string(name, buffer, sizeof(buffer));
	if (!buffer[0])
	{
		return fallback;
	}

	cursor = buffer;
	while (*cursor)
	{
		char *next;

		value = atoi(cursor);
		if (current >= index)
		{
			return value;
		}
		next = BotMoveProbeScheduleNext(cursor);
		if (!next)
		{
			break;
		}
		cursor = next + 1;
		current++;
	}

	return value;
}

static float BotMoveProbeReadScheduleFloat(const char *name, int index, float fallback)
{
	char buffer[2048];
	char *cursor;
	int current = 0;
	float value = fallback;

	if (index < 0)
	{
		return fallback;
	}

	trap_cvar_string(name, buffer, sizeof(buffer));
	if (!buffer[0])
	{
		return fallback;
	}

	cursor = buffer;
	while (*cursor)
	{
		char *next;

		value = atof(cursor);
		if (current >= index)
		{
			return value;
		}
		next = BotMoveProbeScheduleNext(cursor);
		if (!next)
		{
			break;
		}
		cursor = next + 1;
		current++;
	}

	return value;
}

static float BotMoveProbeYawDelta(float to, float from)
{
	float delta = to - from;

	while (delta > 180.0f) delta -= 360.0f;
	while (delta < -180.0f) delta += 360.0f;
	return delta;
}

static float BotMoveProbeBlendYaw(float from, float to, float weight)
{
	weight = bound(0.0f, weight, 1.0f);
	return anglemod(from + (BotMoveProbeYawDelta(to, from) * weight));
}

static void BotLogMoveProbeQwdEvent(gedict_t *self, int slot, const char *event,
									int target_index, int next_index, float distance)
{
	int ednum = NUM_FOR_EDICT(self);
	float active_seconds = 0.0f;

	if (!cvar("k_fb_moveprobe_log_commands") || slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	active_seconds = moveprobe_qwd_active_seconds[slot];
	if (moveprobe_qwd_has_active_since[slot])
	{
		active_seconds = g_globalvars.time - moveprobe_qwd_active_since[slot];
	}

	G_cprint("FBMOVEPROBE_QWD_EVENT time=%.3f ed=%d name=%s event=%s "
			 "target=%d next=%d count=%d distance=%.3f advanced=%d active=%d "
			 "complete=%d active_seconds=%.3f origin=%.3f,%.3f,%.3f\n",
			 g_globalvars.time, ednum, self->netname, event, target_index, next_index,
			 moveprobe_qwd_point_count[slot], distance,
			 moveprobe_qwd_advanced_points[slot], moveprobe_qwd_active[slot],
			 moveprobe_qwd_complete[slot], active_seconds, PASSVEC3(self->s.v.origin));
}

static qbool BotMoveProbeQwdActive(gedict_t *self, int slot, vec3_t route_direction)
{
	vec3_t points[MOVEPROBE_QWD_MAX_POINTS];
	vec3_t delta;
	char expected_map[64];
	float point_radius = cvar("k_fb_moveprobe_qwd_point_radius");
	float start_radius = cvar("k_fb_moveprobe_qwd_start_radius");
	float time_step_ms = cvar("k_fb_moveprobe_qwd_time_step_ms");
	float time_offset_ms = cvar("k_fb_moveprobe_qwd_time_offset_ms");
	float distance = 999999.0f;
	int max_advance_frame = (int)cvar("k_fb_moveprobe_qwd_max_advance_frame");
	int count;
	int index;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return false;
	}

	BotMoveProbePrepareQwdFrame(slot);
	if (moveprobe_qwd_complete[slot])
	{
		moveprobe_qwd_active[slot] = 0;
		return false;
	}
	trap_cvar_string("k_fb_moveprobe_qwd_map", expected_map, sizeof(expected_map));
	if (!expected_map[0])
	{
		strlcpy(expected_map, "dm3", sizeof(expected_map));
	}
	if (strneq(mapname, expected_map))
	{
		BotMoveProbeResetQwdSession(slot);
		return false;
	}

	count = BotMoveProbeReadQwdWaypoints(points);
	moveprobe_qwd_point_count[slot] = count;
	if (count < 2)
	{
		BotMoveProbeResetQwdSession(slot);
		return false;
	}

	if (point_radius <= 0)
	{
		point_radius = 96.0f;
	}
	if (start_radius <= 0)
	{
		start_radius = 192.0f;
	}
	point_radius = bound(24.0f, point_radius, 256.0f);
	start_radius = bound(point_radius, start_radius, 512.0f);

	if (!moveprobe_qwd_active[slot])
	{
		VectorSubtract(points[0], self->s.v.origin, delta);
		distance = VectorLength(delta);
		moveprobe_qwd_distance[slot] = distance;
		if (distance > start_radius)
		{
			return false;
		}
		moveprobe_qwd_active[slot] = 1;
		moveprobe_qwd_point_index[slot] = 0;
		moveprobe_qwd_active_since[slot] = g_globalvars.time;
		moveprobe_qwd_has_active_since[slot] = 1;
		moveprobe_qwd_active_seconds[slot] = 0.0f;
		BotLogMoveProbeQwdEvent(self, slot, "activate", 0, 0, distance);
	}

	index = moveprobe_qwd_point_index[slot];
	index = bound(0, index, count - 1);
	if (time_step_ms > 0.0f)
	{
		int timed_index;

		time_step_ms = bound(1.0f, time_step_ms, 1000.0f);
		timed_index = (int)(((moveprobe_qwd_active_seconds[slot] * 1000.0f) + time_offset_ms) / time_step_ms);
		timed_index = bound(0, timed_index, count - 1);
		VectorSubtract(points[timed_index], self->s.v.origin, delta);
		distance = VectorLength(delta);
		if (timed_index > index)
		{
			moveprobe_qwd_advanced_points[slot] += timed_index - index;
			moveprobe_qwd_advanced_this_frame[slot] += timed_index - index;
			BotLogMoveProbeQwdEvent(self, slot, "time_advance", index, timed_index, distance);
		}
		index = timed_index;
		moveprobe_qwd_point_index[slot] = index;
		moveprobe_qwd_distance[slot] = distance;
		if (index >= count - 1)
		{
			moveprobe_qwd_complete[slot] = 1;
			moveprobe_qwd_active[slot] = 0;
			BotLogMoveProbeQwdEvent(self, slot, "complete", index, index, distance);
			return false;
		}
		VectorSubtract(points[index], self->s.v.origin, route_direction);
		route_direction[2] = 0;
		return VectorNormalize(route_direction) > 0;
	}

	while (index < count)
	{
		float active_point_radius = point_radius;

		if (max_advance_frame > 0
			&& moveprobe_qwd_advanced_this_frame[slot] >= max_advance_frame)
		{
			break;
		}
		active_point_radius = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_qwd_point_radius_schedule",
															index, active_point_radius);
		active_point_radius = bound(8.0f, active_point_radius, 256.0f);
		VectorSubtract(points[index], self->s.v.origin, delta);
		distance = VectorLength(delta);
		if (distance > active_point_radius)
		{
			break;
		}
		if (index >= count - 1)
		{
			moveprobe_qwd_complete[slot] = 1;
			moveprobe_qwd_active[slot] = 0;
			moveprobe_qwd_distance[slot] = distance;
			moveprobe_qwd_point_index[slot] = index;
			BotLogMoveProbeQwdEvent(self, slot, "complete", index, index, distance);
			break;
		}
		{
			int reached_index = index;

			index++;
			moveprobe_qwd_advanced_points[slot]++;
			moveprobe_qwd_advanced_this_frame[slot]++;
			moveprobe_qwd_distance[slot] = distance;
			moveprobe_qwd_point_index[slot] = index;
			BotLogMoveProbeQwdEvent(self, slot, "advance", reached_index, index, distance);
		}
	}

	moveprobe_qwd_point_index[slot] = index;
	moveprobe_qwd_distance[slot] = distance;
	if (moveprobe_qwd_has_active_since[slot])
	{
		moveprobe_qwd_active_seconds[slot] = g_globalvars.time - moveprobe_qwd_active_since[slot];
	}
	if (moveprobe_qwd_complete[slot])
	{
		return false;
	}

	VectorSubtract(points[index], self->s.v.origin, route_direction);
	route_direction[2] = 0;
	return VectorNormalize(route_direction) > 0;
}

static void BotMoveProbeResetReplaySession(int slot)
{
	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	moveprobe_replay_active[slot] = 0;
	moveprobe_replay_complete[slot] = 0;
	moveprobe_replay_cursor[slot] = 0;
	moveprobe_replay_has_start[slot] = 0;
	moveprobe_replay_start_time[slot] = 0.0f;
	moveprobe_replay_has_seen_time[slot] = 0;
	moveprobe_replay_last_seen_time[slot] = 0.0f;
	moveprobe_replay_store_for_slot[slot] = 0;
	moveprobe_s27_handoff_latched[slot] = 0;
	VectorClear(moveprobe_replay_expected[slot]);
	moveprobe_replay_divergence[slot] = 0.0f;
	moveprobe_replay_divergence_h[slot] = 0.0f;
	moveprobe_replay_divergence_v[slot] = 0.0f;
	moveprobe_replay_corr_accum[slot] = 0.0f;
	moveprobe_replay_corr_max[slot] = 0.0f;
	moveprobe_replay_cmd_msec[slot] = 0;
	moveprobe_replay_step_emitted[slot] = 0;
	moveprobe_replay_start_resnaps[slot] = 0;
	moveprobe_replay_attempt_num[slot] = 0;
	moveprobe_replay_attempt_maxh[slot] = 0.0f;
	moveprobe_replay_attempt_break[slot] = -1;
	moveprobe_replay_attempt_finalh[slot] = 0.0f;
	moveprobe_replay_loop_cooldown[slot] = 0.0f;
}

static int BotMoveProbeParseReplayLine(char *line, moveprobe_replay_frame_t *f)
{
	float vals[14];
	char *cursor = line;
	int i;

	for (i = 0; i < 14; i++)
	{
		while ((*cursor == ' ') || (*cursor == '\t'))
		{
			cursor++;
		}
		if ((*cursor == '\0') || (*cursor == '\n') || (*cursor == '\r'))
		{
			return 0;
		}
		vals[i] = atof(cursor);
		while (*cursor && (*cursor != ' ') && (*cursor != '\t')
			&& (*cursor != '\n') && (*cursor != '\r'))
		{
			cursor++;
		}
	}

	f->msec = (int)vals[0];
	f->origin[0] = vals[1];
	f->origin[1] = vals[2];
	f->origin[2] = vals[3];
	f->velocity[0] = vals[4];
	f->velocity[1] = vals[5];
	f->velocity[2] = vals[6];
	f->angles[0] = vals[7];
	f->angles[1] = vals[8];
	f->angles[2] = vals[9];
	f->forwardmove = (int)vals[10];
	f->sidemove = (int)vals[11];
	f->upmove = (int)vals[12];
	f->buttons = (int)vals[13];
	return 1;
}

static int BotMoveProbeLoadReplayFile(moveprobe_replay_store_t *store, const char *fname)
{
	fileHandle_t handle;
	char line[256];
	int count = 0;
	float cumulative = 0.0f;

	handle = std_fropen("%s", fname);
	if (handle < 0)
	{
		return -1;
	}

	while ((count < MOVEPROBE_REPLAY_MAX_FRAMES) && std_fgets(handle, line, sizeof(line)))
	{
		moveprobe_replay_frame_t *f;

		if ((line[0] == '#') || (line[0] == '\0') || (line[0] == '\n') || (line[0] == '\r'))
		{
			continue;
		}

		f = &store->frames[count];
		if (!BotMoveProbeParseReplayLine(line, f))
		{
			continue;
		}
		if (f->msec < 1)
		{
			f->msec = 1;
		}
		cumulative += (float)f->msec;
		f->cumulative_ms = cumulative;
		count++;
	}

	std_fclose(handle);
	return count;
}

// Resolves this bot's replay file with the per-slot cvar convention (#95) and
// returns its loaded store, or NULL. *error_out is set to 1 only when a
// PER-SLOT file failed to load or claim a store (loud-fail contract: caller
// must hold the bot). The global-fallback path keeps the legacy behavior
// (one load_failed row, stock movement continues) byte-identical.
static moveprobe_replay_store_t *BotMoveProbeEnsureReplayLoaded(gedict_t *self, int slot,
																int *error_out)
{
	char fname[128];
	int from_slot;
	int free_index = -1;
	int i;
	moveprobe_replay_store_t *store = NULL;

	*error_out = 0;
	if (slot >= 0 && slot < MAX_CLIENTS)
	{
		moveprobe_replay_store_for_slot[slot] = 0;
	}

	from_slot = BotMoveProbeCvarStringForBot(self, "replay_file", fname, sizeof(fname));
	if (fname[0] == '\0')
	{
		return NULL;
	}

	for (i = 0; i < MOVEPROBE_REPLAY_MAX_FILES; i++)
	{
		if ((moveprobe_replay_stores[i].load_state != 0)
			&& streq(fname, moveprobe_replay_stores[i].file))
		{
			store = &moveprobe_replay_stores[i];
			break;
		}
		if ((free_index < 0) && (moveprobe_replay_stores[i].load_state == 0))
		{
			free_index = i;
		}
	}

	if (!store && (free_index < 0))
	{
		// Every store holds another file; reclaim one no slot currently uses.
		for (i = 0; i < MOVEPROBE_REPLAY_MAX_FILES; i++)
		{
			int s;
			int used = 0;

			for (s = 0; s < MAX_CLIENTS; s++)
			{
				if (moveprobe_replay_store_for_slot[s] == (i + 1))
				{
					used = 1;
					break;
				}
			}
			if (!used)
			{
				free_index = i;
				break;
			}
		}
	}

	if (!store)
	{
		if (free_index < 0)
		{
			if (from_slot)
			{
				BotMoveProbeReportPerSlotError(self, "replay_file", fname,
											   "replay_store_exhausted");
				*error_out = 1;
			}
			return NULL;
		}
		store = &moveprobe_replay_stores[free_index];
		strlcpy(store->file, fname, sizeof(store->file));
		store->count = BotMoveProbeLoadReplayFile(store, fname);
		if (store->count < 2)
		{
			store->load_state = -1;
			store->count = 0;
			G_cprint("FBMOVEPROBE_REPLAY load_failed file=%s\n", fname);
		}
		else
		{
			store->load_state = 1;
			G_cprint("FBMOVEPROBE_REPLAY loaded file=%s frames=%d\n", fname, store->count);
		}
	}

	if (store->load_state != 1)
	{
		if (from_slot)
		{
			BotMoveProbeReportPerSlotError(self, "replay_file", fname, "replay_load_failed");
			*error_out = 1;
		}
		return NULL;
	}

	if (slot >= 0 && slot < MAX_CLIENTS)
	{
		moveprobe_replay_store_for_slot[slot] = (int)(store - moveprobe_replay_stores) + 1;
	}
	return store;
}

static int BotMoveProbeReplayCountForSlot(int slot)
{
	int idx = (slot >= 0 && slot < MAX_CLIENTS) ? moveprobe_replay_store_for_slot[slot] : 0;

	return (idx > 0) ? moveprobe_replay_stores[idx - 1].count : 0;
}

static qbool BotMoveProbeReplayOneShot(void)
{
	return ((int)cvar("k_fb_moveprobe_replay_one_shot")) != 0;
}

static void BotLogMoveProbeReplayEvent(gedict_t *self, int slot, const char *event)
{
	int ednum = NUM_FOR_EDICT(self);

	if (!cvar("k_fb_moveprobe_log_commands") || slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	G_cprint("FBMOVEPROBE_REPLAY_EVENT time=%.3f ed=%d name=%s event=%s "
			 "cursor=%d count=%d divergence=%.3f divergence_h=%.3f divergence_v=%.3f "
			 "origin=%.3f,%.3f,%.3f expected=%.3f,%.3f,%.3f\n",
			 g_globalvars.time, ednum, self->netname, event,
			 moveprobe_replay_cursor[slot], BotMoveProbeReplayCountForSlot(slot),
			 moveprobe_replay_divergence[slot], moveprobe_replay_divergence_h[slot],
			 moveprobe_replay_divergence_v[slot], PASSVEC3(self->s.v.origin),
			 PASSVEC3(moveprobe_replay_expected[slot]));
}

// Ends one replay attempt: logs a per-attempt summary line, then either re-arms
// for the next attempt (loop mode -- the Python harness tails FBMOVEPROBE_ATTEMPT
// to score success rate and auto-tune cvars live) or marks the session complete.
static void BotMoveProbeEndAttempt(gedict_t *self, int slot, int count, const char *reason)
{
	if (cvar("k_fb_moveprobe_log_commands"))
	{
		G_cprint("FBMOVEPROBE_ATTEMPT ed=%d n=%d end=%s break_cursor=%d furthest_cursor=%d "
				 "frame_count=%d maxH=%.1f final_maxH=%.1f corr_accum=%.1f corr_max=%.1f\n",
				 NUM_FOR_EDICT(self), moveprobe_replay_attempt_num[slot], reason,
				 moveprobe_replay_attempt_break[slot], moveprobe_replay_cursor[slot], count,
				 moveprobe_replay_attempt_maxh[slot], moveprobe_replay_attempt_finalh[slot],
				 moveprobe_replay_corr_accum[slot], moveprobe_replay_corr_max[slot]);
	}

	if ((int)cvar("k_fb_moveprobe_replay_loop"))
	{
		// Re-arm: clear active/complete so the next frame re-snaps to frame 0 and
		// starts a fresh attempt. Keep the cooldown under stale_gap (1s) so the
		// per-frame stale-session reset does not fire during the pause.
		float cd = cvar("k_fb_moveprobe_loop_cooldown");
		if (cd <= 0)
		{
			cd = 0.5f;
		}
		cd = bound(0.0f, cd, 0.9f);
		moveprobe_replay_active[slot] = 0;
		moveprobe_replay_complete[slot] = 0;
		moveprobe_replay_cursor[slot] = 0;
		moveprobe_replay_has_start[slot] = 0;
		moveprobe_replay_loop_cooldown[slot] = g_globalvars.time + cd;
	}
	else
	{
		moveprobe_replay_complete[slot] = 1;
		moveprobe_replay_active[slot] = 0;
	}
}

// Mode 10: open-loop replay. Snap to the human frame-0 state once, then emit the
// exact human usercmd for the resampled time index each frame. forwardmove /
// sidemove / upmove are already view-relative in a usercmd, so they pass straight
// through with the human view angles -- no projection. Divergence (bot origin vs
// human origin at the same time index) is recorded for the finding.
// replay_variant: 0 = open-loop replay (mode 10), 1 = closed-loop steering
// (mode 11), 2 = corrective replay (mode 12). The snap, replay timeline,
// divergence and completion are shared so every variant is scored identically.

static void BotMoveProbeReplayGrantAttackLoadout(gedict_t *self)
{
	int rockets;

	if (cvar("k_fb_moveprobe_replay_attack") <= 0.0f)
	{
		return;
	}
	if (cvar("k_fb_moveprobe_replay_attack_grant") <= 0.0f)
	{
		return;
	}

	rockets = (int)cvar("k_fb_moveprobe_replay_attack_rockets");
	if (rockets <= 0)
	{
		rockets = 50;
	}

	self->s.v.items = (int)self->s.v.items | IT_ROCKET_LAUNCHER | IT_ROCKETS;
	if (self->s.v.ammo_rockets < rockets)
	{
		self->s.v.ammo_rockets = rockets;
	}
	self->s.v.weapon = IT_ROCKET_LAUNCHER;
	self->s.v.currentammo = self->s.v.ammo_rockets;
}

static qbool BotApplyMoveProbeReplay(gedict_t *self, int slot, qbool *jumping, qbool *firing, int *impulse, vec3_t direction, int replay_variant)
{
	float stale_gap = cvar("k_fb_moveprobe_replay_stale_gap");
	int count;
	int cursor;
	float elapsed_ms;
	moveprobe_replay_frame_t *f;
	moveprobe_replay_frame_t interp_frame;
	moveprobe_replay_frame_t *frames;
	moveprobe_replay_store_t *store;
	int load_error = 0;
	qbool one_shot;
	float clock_back = 0.0f;
	vec3_t delta;

	if (slot < 0 || slot >= MAX_CLIENTS)
	{
		return false;
	}
	if (stale_gap <= 0.0f)
	{
		stale_gap = 1.0f;
	}
	stale_gap = bound(0.1f, stale_gap, 120.0f);
	one_shot = BotMoveProbeReplayOneShot();

	store = BotMoveProbeEnsureReplayLoaded(self, slot, &load_error);
	if (load_error)
	{
		// Per-slot replay file misconfigured (#95): loud-fail contract -- hold
		// the bot at spawn instead of silently falling back to stock movement.
		BotMoveProbeResetReplaySession(slot);
		VectorClear(direction);
		*jumping = false;
		*firing = false;
		*impulse = 0;
		return true;
	}
	if (!store || (store->count < 2))
	{
		BotMoveProbeResetReplaySession(slot);
		return false;
	}
	frames = store->frames;
	count = store->count;
	{
		// Gate the replay to the map its .cmds was recorded on. The map is named
		// by k_fb_moveprobe_replay_map; empty means no restriction (operator
		// guarantees the right .cmds for the loaded map).
		char want_map[64];
		trap_cvar_string("k_fb_moveprobe_replay_map", want_map, sizeof(want_map));
		if (want_map[0] && strneq(mapname, want_map))
		{
			BotMoveProbeResetReplaySession(slot);
			return false;
		}
	}

	if (moveprobe_replay_has_seen_time[slot])
	{
		clock_back = moveprobe_replay_last_seen_time[slot] - g_globalvars.time;
	}
	if (!moveprobe_replay_has_seen_time[slot]
		|| (clock_back > 0.25f)
		|| ((g_globalvars.time - moveprobe_replay_last_seen_time[slot]) > stale_gap))
	{
		BotMoveProbeResetReplaySession(slot);
	}
	moveprobe_replay_has_seen_time[slot] = 1;
	moveprobe_replay_last_seen_time[slot] = g_globalvars.time;
	// A reset above also released the store reference; re-assert it so this
	// frame's logging and another slot's reclaim scan both see the truth.
	moveprobe_replay_store_for_slot[slot] = (int)(store - moveprobe_replay_stores) + 1;

	if (moveprobe_replay_complete[slot])
	{
		moveprobe_replay_active[slot] = 0;
		if (one_shot)
		{
			VectorClear(direction);
			*jumping = false;
			*firing = false;
			*impulse = 0;
			return true;
		}
		return false;
	}

	// Loop mode: after an attempt ends we pause briefly before re-arming.
	if (!moveprobe_replay_active[slot] && (g_globalvars.time < moveprobe_replay_loop_cooldown[slot]))
	{
		return false;
	}

	if (!moveprobe_replay_active[slot])
	{
		f = &frames[0];
		setorigin(self, PASSVEC3(f->origin));
		VectorCopy(f->velocity, self->s.v.velocity);
		VectorCopy(f->angles, self->s.v.angles);
		VectorCopy(f->angles, self->fb.desired_angle);
		self->fb.desired_angle[ROLL] = 0;
		self->s.v.fixangle = 1;

		moveprobe_replay_active[slot] = 1;
		moveprobe_replay_cursor[slot] = 0;
		moveprobe_replay_start_time[slot] = g_globalvars.time;
		moveprobe_replay_has_start[slot] = 1;
		VectorCopy(f->origin, moveprobe_replay_expected[slot]);
		moveprobe_replay_divergence[slot] = 0.0f;
		moveprobe_replay_divergence_h[slot] = 0.0f;
		moveprobe_replay_divergence_v[slot] = 0.0f;
		// Fresh attempt: bump the counter and clear per-attempt accumulators.
		moveprobe_replay_attempt_num[slot]++;
		moveprobe_replay_attempt_maxh[slot] = 0.0f;
		moveprobe_replay_attempt_break[slot] = -1;
		moveprobe_replay_attempt_finalh[slot] = 0.0f;
		moveprobe_replay_corr_accum[slot] = 0.0f;
		moveprobe_replay_corr_max[slot] = 0.0f;
		moveprobe_replay_step_emitted[slot] = 0;
		moveprobe_replay_start_resnaps[slot] = 0;
		// Mode 21 (variant 3) fresh-attempt state: start at the first real waypoint,
		// no previous origin yet, undecided weave sign.
		moveprobe_s21_wp[slot] = 1;
		moveprobe_s21_has_prev[slot] = 0;
		moveprobe_s21_teleported[slot] = 0;
		moveprobe_s21_descend_latch[slot] = 0;
		moveprobe_accel_strafe_sign[slot] = 0;
		// Mode 22 (variant 4) fresh-attempt state: re-seed sim_yaw lazily, undecided sign.
		moveprobe_s22_init[slot] = 0;
		moveprobe_s22_strafe_sign[slot] = 0;
		moveprobe_s22_flip_t[slot] = g_globalvars.time;
		BotMoveProbeReplayGrantAttackLoadout(self);
		BotLogMoveProbeReplayEvent(self, slot, "activate");
	}

	if (cvar("k_fb_moveprobe_replay_step_cursor") > 0.0f)
	{
		int step = (int)cvar("k_fb_moveprobe_replay_step_cursor");

		if (step <= 0)
		{
			step = 1;
		}
		step = bound(1, step, 8);
		cursor = moveprobe_replay_cursor[slot];
		if (moveprobe_replay_step_emitted[slot])
		{
			cursor += step;
		}
		else
		{
			moveprobe_replay_step_emitted[slot] = 1;
		}
		if (cursor > count - 1)
		{
			cursor = count - 1;
		}
		elapsed_ms = frames[cursor].cumulative_ms;
	}
	else
	{
		elapsed_ms = (g_globalvars.time - moveprobe_replay_start_time[slot]) * 1000.0f;
		cursor = moveprobe_replay_cursor[slot];
		while ((cursor < count - 1) && (frames[cursor].cumulative_ms <= elapsed_ms))
		{
			cursor++;
		}
	}
	moveprobe_replay_cursor[slot] = cursor;

	if (cursor >= count - 1)
	{
		f = &frames[count - 1];
		VectorCopy(f->origin, moveprobe_replay_expected[slot]);
		VectorSubtract(self->s.v.origin, f->origin, delta);
		moveprobe_replay_divergence[slot] = VectorLength(delta);
		moveprobe_replay_divergence_h[slot] = sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
		moveprobe_replay_divergence_v[slot] = fabs(delta[2]);
		if (moveprobe_replay_divergence_h[slot] > moveprobe_replay_attempt_maxh[slot])
		{
			moveprobe_replay_attempt_maxh[slot] = moveprobe_replay_divergence_h[slot];
		}
		if (moveprobe_replay_divergence_h[slot] > moveprobe_replay_attempt_finalh[slot])
		{
			moveprobe_replay_attempt_finalh[slot] = moveprobe_replay_divergence_h[slot];
		}
		BotLogMoveProbeReplayEvent(self, slot, "complete");
		BotMoveProbeEndAttempt(self, slot, count, "complete");
		return false;
	}

	f = &frames[cursor];
	if ((cvar("k_fb_moveprobe_replay_interpolate") > 0.0f) && (cursor > 0))
	{
		moveprobe_replay_frame_t *a = &frames[cursor - 1];
		moveprobe_replay_frame_t *b = &frames[cursor];
		float span = b->cumulative_ms - a->cumulative_ms;

		if (span > 0.0f)
		{
			float frac = bound(0.0f, (elapsed_ms - a->cumulative_ms) / span, 1.0f);
			float dyaw;

			interp_frame = *b;
			interp_frame.origin[0] = a->origin[0] + (b->origin[0] - a->origin[0]) * frac;
			interp_frame.origin[1] = a->origin[1] + (b->origin[1] - a->origin[1]) * frac;
			interp_frame.origin[2] = a->origin[2] + (b->origin[2] - a->origin[2]) * frac;
			interp_frame.velocity[0] = a->velocity[0] + (b->velocity[0] - a->velocity[0]) * frac;
			interp_frame.velocity[1] = a->velocity[1] + (b->velocity[1] - a->velocity[1]) * frac;
			interp_frame.velocity[2] = a->velocity[2] + (b->velocity[2] - a->velocity[2]) * frac;
			interp_frame.angles[PITCH] = a->angles[PITCH] + (b->angles[PITCH] - a->angles[PITCH]) * frac;
			dyaw = anglemod(b->angles[YAW]) - anglemod(a->angles[YAW]);
			while (dyaw > 180.0f)
			{
				dyaw -= 360.0f;
			}
			while (dyaw < -180.0f)
			{
				dyaw += 360.0f;
			}
			interp_frame.angles[YAW] = anglemod(a->angles[YAW] + dyaw * frac);
			interp_frame.angles[ROLL] = a->angles[ROLL] + (b->angles[ROLL] - a->angles[ROLL]) * frac;
			f = &interp_frame;
		}
	}
	if (cvar("k_fb_moveprobe_replay_use_recorded_msec") > 0.0f)
	{
		moveprobe_replay_cmd_msec[slot] = bound(1, f->msec, 255);
	}
	VectorCopy(f->origin, moveprobe_replay_expected[slot]);
	VectorSubtract(self->s.v.origin, f->origin, delta);
	moveprobe_replay_divergence[slot] = VectorLength(delta);
	moveprobe_replay_divergence_h[slot] = sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
	moveprobe_replay_divergence_v[slot] = fabs(delta[2]);
	if (cvar("k_fb_moveprobe_replay_start_resnap") > 0.0f)
	{
		int resnap_cursor = (int)cvar("k_fb_moveprobe_replay_start_resnap_cursor");
		float resnap_dist = cvar("k_fb_moveprobe_replay_start_resnap_dist");

		if (resnap_cursor <= 0)
		{
			resnap_cursor = 16;
		}
		if (resnap_dist <= 0.0f)
		{
			resnap_dist = 256.0f;
		}
		if ((moveprobe_replay_start_resnaps[slot] <= 0)
			&& (cursor <= resnap_cursor)
			&& (moveprobe_replay_divergence_h[slot] > resnap_dist))
		{
			setorigin(self, PASSVEC3(f->origin));
			VectorCopy(f->velocity, self->s.v.velocity);
			VectorCopy(f->angles, self->s.v.angles);
			VectorCopy(f->angles, self->fb.desired_angle);
			self->fb.desired_angle[ROLL] = 0;
			self->s.v.fixangle = 1;
			moveprobe_replay_start_resnaps[slot]++;
			VectorCopy(f->origin, moveprobe_replay_expected[slot]);
			VectorSubtract(self->s.v.origin, f->origin, delta);
			moveprobe_replay_divergence[slot] = VectorLength(delta);
			moveprobe_replay_divergence_h[slot] = sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
			moveprobe_replay_divergence_v[slot] = fabs(delta[2]);
			BotLogMoveProbeReplayEvent(self, slot, "start_resnap");
		}
	}
	if (cvar("k_fb_moveprobe_replay_attack") > 0.0f)
	{
		int attack_impulse = (int)cvar("k_fb_moveprobe_replay_attack_impulse");
		if (attack_impulse <= 0)
		{
			attack_impulse = 7;
		}
		*impulse = attack_impulse;
		if (f->buttons & 1)
		{
			*firing = true;
		}
		else
		{
			*firing = false;
		}
	}

	{
		// Per-attempt tracking + blowup early-abort. corridor = the lock-on
		// threshold (break_cursor = where divH first leaves the corridor);
		// final segment (>= final_start) feeds the "landed the last jump" check;
		// kill_div ends a hopeless attempt early so the loop retries sooner.
		float dh = moveprobe_replay_divergence_h[slot];
		float corridor = cvar("k_fb_moveprobe_corridor");
		float kill = cvar("k_fb_moveprobe_kill_div");
		int final_start = (int)cvar("k_fb_moveprobe_final_start");

		if (corridor <= 0)
		{
			corridor = 64.0f;
		}
		if (dh > moveprobe_replay_attempt_maxh[slot])
		{
			moveprobe_replay_attempt_maxh[slot] = dh;
		}
		if ((moveprobe_replay_attempt_break[slot] < 0) && (dh > corridor))
		{
			moveprobe_replay_attempt_break[slot] = cursor;
		}
		if ((cursor >= final_start) && (dh > moveprobe_replay_attempt_finalh[slot]))
		{
			moveprobe_replay_attempt_finalh[slot] = dh;
		}
		if ((kill > 0) && (dh > kill))
		{
			BotMoveProbeEndAttempt(self, slot, count, "blowup");
			return false;
		}
	}

	if (replay_variant == 0)
	{
		// Mode 10: open-loop replay -- emit the exact human usercmd. The view
		// angles and view-relative forward/side/up pass straight through.
		VectorCopy(f->angles, self->fb.desired_angle);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = f->forwardmove;
		direction[1] = f->sidemove;
		direction[2] = f->upmove;
		*jumping = (f->buttons & 2) ? true : false;

		return true;
	}

	// Mode 11: closed-loop steering. Same replay timeline, snap, divergence and
	// completion as mode 10 (all computed above) so the divH-vs-baseline A/B is
	// exact -- the ONLY difference is the per-frame command. Instead of replaying
	// the human's stale usercmd we re-derive the heading from the bot's ACTUAL
	// current origin toward the human origin `lookahead` frames ahead, then move
	// forward + strafe (sign taken from the human's recorded sidemove) and jump.
	// Drift is corrected by recomputing the aim every frame rather than
	// integrating a usercmd against a body that has already diverged.
	if (replay_variant == 1)
	{
		int lookahead = (int)cvar("k_fb_moveprobe_lookahead_frames");
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");
		float sidemove = cvar("k_fb_moveprobe_sidemove");
		float strafe_sign;
		int target_index;
		moveprobe_replay_frame_t *target;
		vec3_t route_direction;

		if (lookahead <= 0)
		{
			lookahead = 4;
		}
		lookahead = bound(1, lookahead, 64);
		if (forwardmove <= 0)
		{
			forwardmove = 800;
		}
		if (sidemove < 0)
		{
			sidemove = -sidemove;
		}
		if (sidemove == 0)
		{
			sidemove = 508;
		}

		target_index = cursor + lookahead;
		if (target_index > count - 1)
		{
			target_index = count - 1;
		}
		target = &frames[target_index];

		VectorSubtract(target->origin, self->s.v.origin, route_direction);
		route_direction[2] = 0;
		if (VectorNormalize(route_direction) <= 0)
		{
			// Bot sits on the lookahead target this frame; fall back to the
			// exact human usercmd rather than steer from a degenerate heading.
			VectorCopy(f->angles, self->fb.desired_angle);
			self->fb.desired_angle[ROLL] = 0;
			direction[0] = f->forwardmove;
			direction[1] = f->sidemove;
			direction[2] = f->upmove;
			*jumping = (f->buttons & 2) ? true : false;
			return true;
		}

		// Aim along travel, so view-relative forward/side equal route forward/side
		// (the trap_makevectors projection used by modes 5-9 is identity here).
		strafe_sign = (f->sidemove < 0) ? -1.0f : 1.0f;
		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(route_direction);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = forwardmove;
		direction[1] = sidemove * strafe_sign;
		direction[2] = cvar("k_fb_moveprobe_upmove");
		*jumping = true;

		return true;
	}

	if (replay_variant == 3)
	{
		// Mode 21: GENERATIVE route-follow = mode-20's low-numerator (cs->0) air
		// accel, made DIRECTED. We reuse the replay loader only for (a) the frame-0
		// snap that starts the bot at the human's SNG origin and (b) the human path
		// as a waypoint list. Per AIR frame we accelerate exactly like mode 20 but
		// pick the curl SIGN to net-track the bearing to the next path waypoint
		// (advanced by PROXIMITY, not time), with a hysteresis swing so the weave is
		// a well-timed back-and-forth across the goal heading (not a per-frame buzz),
		// plus a mode-20-style +/-45 deg open-side override so it never weaves into a
		// wall. The SNG teleporter is handled by snapping the waypoint cursor forward
		// to the nearest path point after the bot's origin jumps (it teleported).
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float look = cvar("k_fb_moveprobe_s19_wall");
		float advance_r = cvar("k_fb_moveprobe_s21_advance");
		float swing = cvar("k_fb_moveprobe_s21_swing");
		float turn_thresh = cvar("k_fb_moveprobe_s21_turn");
		float corner_aim = cvar("k_fb_moveprobe_s21_corner_aim");
		float corner_thresh = cvar("k_fb_moveprobe_s21_corner_thresh");
		float sv_maxspeed = cvar("sv_maxspeed");
		float pp_xtrack = cvar("k_fb_moveprobe_s21_pp_xtrack");
		float pp_start = cvar("k_fb_moveprobe_s21_pp_start");
		int wp, scan, sign, i, best, gate, near_idx, carrot, look_fr;
		float hor_speed_sq, rotation, goal_yaw, vel_yaw, signed_to_goal, bestd, dd;
		float to_goal, herr, corner_mag, crosstrack;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump, home_straight, final_leg, descend, hard_corner;
		vec3_t cur_dir, to_wp, proposed_dir, moved, d;
		vec3_t left_dir, right_dir, lp, rp, fp;
		vec3_t up_vector = { 0, 0, 1 };
		float left_open, right_open, fwd_open;

		if (numerator <= 0) numerator = 9.0f;
		if (bootstrap_deg <= 0) bootstrap_deg = 25.0f;
		if (look <= 0) look = 500.0f;
		if (advance_r <= 0) advance_r = 110.0f;
		if (swing <= 0) swing = 12.0f;
		if (turn_thresh <= 0) turn_thresh = 35.0f;
		// Corner-aim cap (deg): in the CORNER regime the wishdir is aimed TOWARD the
		// goal by up to this many degrees off the velocity heading, instead of the
		// cs->0 perpendicular (~88 deg) wishdir. cs->0 maximizes |v| gain but barely
		// rotates the velocity vector, so at ~470 qu/s the bot ran cs->0 straight past
		// the SE->north hairpin along the south wall (the binding cross-track blocker).
		// Aiming partway at the goal trades some top speed for the turn-rate needed to
		// actually round the bend -- the human's own balanced strafe-turn.
		if (corner_aim <= 0) corner_aim = 68.0f;
		// Aim-at-goal engages ONLY above this (higher) heading-error threshold -- i.e.
		// genuinely sharp, sustained bends like the SE->north hairpin (~90 deg error),
		// NOT the gentle southern-dip/SE curves or transient weave overshoots (which
		// the moderate turn_thresh sustained-sign + cs->0 weave already handle well).
		// Firing aim-at-goal on every >turn_thresh excursion bled speed everywhere and
		// wrecked the mid-route landmarks.
		if (corner_thresh <= 0) corner_thresh = 58.0f;
		// Pure-pursuit cross-track gate (qu): above this lateral error from the human
		// line, shrink the carrot look-ahead so the bot aims nearly perpendicular back
		// onto the line (converge); below it, look further ahead for speed.
		if (pp_xtrack <= 0) pp_xtrack = 150.0f;
		// Pure-pursuit engages only PAST this route fraction -- i.e. right after the SE
		// corner (~0.54), where the eastward drift toward the northward turn begins. The
		// teleporter-exit -> southern-dip -> SE-corner stretch (sharp bends) stays on the
		// proven proximity-advance + corner-weave, which a tight carrot would stall.
		if (pp_start <= 0) pp_start = 0.52f;
		if (sv_maxspeed <= 0) sv_maxspeed = 320.0f;

		wp = moveprobe_s21_wp[slot];
		if (wp < 1) wp = 1;

		// Find the route teleporter gate: the last frame before the path's big origin
		// jump (the teleporter ENTRANCE). The bot must physically touch that trigger
		// to teleport, so we hold the waypoint cursor here until it does -- otherwise
		// proximity-advance aims past the entrance and the bot misses the teleporter.
		gate = -1;
		for (i = 1; i < count; i++)
		{
			VectorSubtract(frames[i].origin,
						   frames[i - 1].origin, d);
			if (DotProduct(d, d) > 200.0f * 200.0f) { gate = i - 1; break; }
		}

		// Teleport detect: a big one-frame origin jump means the bot went through the
		// teleporter -> mark it passed and resync the waypoint cursor to the nearest
		// path point at the bot's new origin (stop aiming back at pre-tele waypoints).
		VectorSubtract(self->s.v.origin, moveprobe_s21_prevorg[slot], moved);
		if (moveprobe_s21_has_prev[slot] && VectorLength(moved) > 200.0f)
		{
			moveprobe_s21_teleported[slot] = 1;
			best = wp;
			bestd = 1e18f;
			for (i = wp; i < count; i++)
			{
				VectorSubtract(frames[i].origin, self->s.v.origin, d);
				d[2] = 0;
				dd = d[0] * d[0] + d[1] * d[1];
				if (dd < bestd) { bestd = dd; best = i; }
			}
			wp = best + 1;
			if (wp > count - 1) wp = count - 1;
			// VARIANCE FIX: pin the post-teleport weave to a DETERMINISTIC initial curl.
			// Run-to-run, the teleporter exit heading bifurcates (~35 deg good vs ~48 deg
			// bad) purely on which way the first post-exit weave curls; the good (route-
			// completing) attempts all started on one sign. Forcing it removes the single
			// biggest downstream-direction variance (bot turning south into the stray
			// teleporter vs north toward the RL ledge).
			moveprobe_accel_strafe_sign[slot] = -1;
		}
		VectorCopy(self->s.v.origin, moveprobe_s21_prevorg[slot]);
		moveprobe_s21_has_prev[slot] = 1;

		// Advance the target waypoint by horizontal proximity.
		scan = 0;
		while (wp < count - 1 && scan < 512)
		{
			VectorSubtract(frames[wp].origin, self->s.v.origin, to_wp);
			to_wp[2] = 0;
			if (to_wp[0] * to_wp[0] + to_wp[1] * to_wp[1] <= advance_r * advance_r)
			{
				wp++;
				scan++;
			}
			else
			{
				break;
			}
		}

		// Hold the cursor at the teleporter entrance until the bot has teleported, so
		// it homes into the trigger instead of running past it on the low floor.
		if (gate >= 1 && !moveprobe_s21_teleported[slot] && wp > gate) wp = gate;
		moveprobe_s21_wp[slot] = wp;

		// PURE PURSUIT for the WHOLE post-teleport leg (was gated to wp>0.65*count, which
		// fired far too late -- by then the bot had already drifted ~260 qu east of the
		// human line and slammed the east wall at x~1467 short of the northward turn).
		// Proximity-advance alone lets the cursor run ahead (NE) while the bot drifts off
		// the line, so the bot cuts the corner east. Instead, from teleporter exit onward
		// track a "carrot" a few frames ahead of the NEAREST human path point, with a
		// cross-track-ADAPTIVE look-ahead: far off the line -> aim nearly at the nearest
		// point (perpendicular convergence); on the line -> look further ahead for speed.
		final_leg = (moveprobe_s21_teleported[slot] && wp > (int)(pp_start * count)) ? true : false;

		// PIT DESCENT is POSITION-based, not route%%-based. The RL nook floor is z=-88;
		// the human falls in BALLISTICALLY off a ledge (top z~0..20 around x 1480..1600,
		// y 90..260) -- a single jump, then free-fall. The old wp>0.85*count gate fired
		// only once the human was already ON the nook floor (idx ~588) -- far too late, so
		// the bot skimmed over (still hopping) or overshot east into a z=-264 sub-pit.
		// Latch the descent the moment the bot reaches the ledge region at/above ledge
		// height; from there gravity + existing horizontal speed arcs it into the nook.
		// Sticky: once committed, stay committed (else it releases mid-fall and re-jumps).
		// Latch as soon as the bot is on the floor past the northward turn (high x, any y
		// past the turn, at/above the z=-24 main floor). MEASURED BEST: this lower-floor
		// latch doubles as the northward-TURN cornering aid -- committing to RL here
		// (aim north-east at the nook) + grounding (press_jump=false) ground-accel-turns
		// the bot hard north exactly at the hairpin. Raising the z gate to the ledge top
		// (z>0) removed that assist and the bot overshot the turn again (57-62% vs 73%).
		// Trade-off accepted: grounded, the bot can't hop the ledge STEP so it tops out
		// at the ledge base (~73% route) -- the remaining final-leg gap (see memory).
		// Latch at x>1455: descend's AIM-at-nook steers the bot NE onto the launch
		// edge (navigation) -- keep this. (Gating the whole latch to x>1505 removed
		// the steering and the bot wandered south into the wrong void: all LEFT_ROUTE,
		// run 20260609T172340Z.) The launch BRAKE is fixed at the jump-suppression
		// line instead (don't ground/de-hop on the launch side; see below).
		if (moveprobe_s21_teleported[slot]
			&& self->s.v.origin[0] > 1455.0f
			&& self->s.v.origin[1] > -90.0f
			&& self->s.v.origin[2] > -35.0f)
			moveprobe_s21_descend_latch[slot] = 1;
		descend = moveprobe_s21_descend_latch[slot] ? true : false;

		near_idx = wp;
		if (final_leg)
		{
			// Search a window straddling the cursor (incl. BEHIND it, so an overshoot
			// can be pulled back) for the nearest human path point.
			bestd = 1e18f;
			best = (wp - 24 < 1) ? 1 : wp - 24;
			for (i = best; i < count && i < wp + 128; i++)
			{
				VectorSubtract(frames[i].origin, self->s.v.origin, d);
				d[2] = 0;
				dd = d[0] * d[0] + d[1] * d[1];
				if (dd < bestd) { bestd = dd; near_idx = i; }
			}
			crosstrack = sqrt(bestd);
			// Cross-track-adaptive look-ahead: when far off the line, aim almost AT the
			// nearest point so the heading points perpendicular back onto the line (the
			// only way to kill a large lateral error at speed); when on the line, look
			// further ahead so the aim is tangent and fast.
			look_fr = (crosstrack > pp_xtrack) ? 8 : 16;
			carrot = near_idx + look_fr;
			if (carrot > count - 1) carrot = count - 1;
			// Once committed to the pit, do NOT let the look-ahead carrot point at a human
			// frame still up on the approach ledge / east of the fall line -- that east
			// pull is what dragged the bot to x~1966 and over/past the nook. Aim straight
			// at the final RL waypoint instead.
			if (descend) carrot = count - 1;
			// Drive wp to the carrot directly (allow pulling BACK on overshoot) -- the
			// teleporter-gate hold above only applies pre-teleport, so this is safe.
			wp = carrot;
			moveprobe_s21_wp[slot] = wp;
		}

		// Heading to the target waypoint.
		VectorSubtract(frames[wp].origin, self->s.v.origin, to_wp);
		to_wp[2] = 0;
		if (VectorNormalize(to_wp) <= 0) { to_wp[0] = 1; to_wp[1] = 0; to_wp[2] = 0; }
		goal_yaw = vectoyaw(to_wp);

		// Current velocity heading.
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];
		if (VectorNormalize(cur_dir) <= 0) { VectorCopy(to_wp, cur_dir); }
		vel_yaw = vectoyaw(cur_dir);

		// Horizontal distance to the FINAL waypoint (RL). Drives the home-stretch
		// behaviour (aim straight + stay grounded) for both the aim branch below and
		// the jump decision later, so the descent into the low RL nook converges
		// instead of the accel-weave skirting past it.
		VectorSubtract(frames[count - 1].origin, self->s.v.origin, d);
		d[2] = 0;
		to_goal = sqrt(d[0] * d[0] + d[1] * d[1]);
		// Within 320 qu of the RL, switch the AIM to straight (point right at the nook
		// and converge) -- but keep BUNNYHOPPING (see jump logic) so the bot flies in at
		// speed instead of grounding early and stalling short. Grounding to settle is a
		// separate, tighter zone (to_goal < 130) so only the last ~130 qu is slow.
		home_straight = (to_goal < 256.0f) ? true : false;

		if (onground || home_straight || descend || (gate >= 1 && !moveprobe_s21_teleported[slot] && wp == gate))
		{
			// On the ground (accel redirects fully), homing into the teleporter, in the
			// pit-descent (straight-aim into the low RL nook), OR on the final approach to
			// the RL: aim STRAIGHT at the target with no weave, so
			// the trajectory crosses the trigger / settles into the nook precisely
			// instead of the wide accel-weave skirting past it.
			VectorCopy(to_wp, proposed_dir);
			// But if that straight line runs into a near wall, curl toward whichever
			// +/-30 deg side is more open -- otherwise an off-line approach jams into the
			// corner just shy of the small floor trigger and stalls at speed 0 with no
			// recovery (the cold-attempt teleporter miss). The grounded/gate-homing path
			// previously had NO wall avoidance at all.
			VectorMA(self->s.v.origin, look, to_wp, fp);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(fp), false, self);
			if (g_globalvars.trace_fraction < 0.5f)
			{
				RotatePointAroundVector(left_dir, up_vector, to_wp, 30.0f);
				RotatePointAroundVector(right_dir, up_vector, to_wp, -30.0f);
				VectorMA(self->s.v.origin, look, left_dir, lp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(lp), false, self);
				left_open = g_globalvars.trace_fraction;
				VectorMA(self->s.v.origin, look, right_dir, rp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(rp), false, self);
				right_open = g_globalvars.trace_fraction;
				if (left_open >= right_open) { VectorCopy(left_dir, proposed_dir); }
				else { VectorCopy(right_dir, proposed_dir); }
				proposed_dir[2] = 0;
				if (VectorNormalize(proposed_dir) <= 0) { VectorCopy(to_wp, proposed_dir); }
			}
		}
		else
		{
			// PRIMARY: goal-tracking weave sign with hysteresis. Hold the curl until
			// velocity swings past the goal bearing by swing deg, then flip -> a
			// controlled back-and-forth of amplitude ~swing centred on the bearing to
			// the next waypoint, so the weave NET-PROGRESSES along the route.
			signed_to_goal = goal_yaw - vel_yaw;
			while (signed_to_goal > 180.0f) signed_to_goal -= 360.0f;
			while (signed_to_goal < -180.0f) signed_to_goal += 360.0f;
			hard_corner = false;
			if (signed_to_goal > turn_thresh || signed_to_goal < -turn_thresh)
			{
				// CORNER: heading error is large -> commit a SUSTAINED turn toward the
				// goal (no weave flip), so the bot tracks bends instead of overshooting.
				sign = (signed_to_goal >= 0.0f) ? 1 : -1;
				moveprobe_accel_strafe_sign[slot] = sign;
				// SHARP corner only (hairpin): additionally aim the wishdir AT the goal
				// (hard_corner) to rotate velocity hard, vs cs->0 running straight past.
				if (signed_to_goal > corner_thresh || signed_to_goal < -corner_thresh)
					hard_corner = true;
			}
			else
			{
				// STRAIGHT: roughly aligned -> weave with hysteresis, holding the curl
				// until velocity swings past the goal by swing deg, then flipping.
				if (moveprobe_accel_strafe_sign[slot] == 0)
					moveprobe_accel_strafe_sign[slot] = (signed_to_goal >= 0.0f) ? 1 : -1;
				if (moveprobe_accel_strafe_sign[slot] > 0 && signed_to_goal < -swing)
					moveprobe_accel_strafe_sign[slot] = -1;
				else if (moveprobe_accel_strafe_sign[slot] < 0 && signed_to_goal > swing)
					moveprobe_accel_strafe_sign[slot] = 1;
				sign = moveprobe_accel_strafe_sign[slot];
			}

			// SAFETY NET only: if the travel direction (velocity) runs into a near
			// wall, curl toward whichever +/-45 side is more open. This does NOT fire
			// in open space, so it no longer hijacks the route-following weave.
			VectorMA(self->s.v.origin, look, cur_dir, fp);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(fp), false, self);
			fwd_open = g_globalvars.trace_fraction;
			if (fwd_open < 0.35f)
			{
				RotatePointAroundVector(left_dir, up_vector, cur_dir, 45.0f);
				RotatePointAroundVector(right_dir, up_vector, cur_dir, -45.0f);
				VectorMA(self->s.v.origin, look, left_dir, lp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(lp), false, self);
				left_open = g_globalvars.trace_fraction;
				VectorMA(self->s.v.origin, look, right_dir, rp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(rp), false, self);
				right_open = g_globalvars.trace_fraction;
				sign = (left_open >= right_open) ? 1 : -1;
				moveprobe_accel_strafe_sign[slot] = sign;
				hard_corner = false;  // wall avoidance wins: curl to the open side, not the goal
			}

			if (hard_corner)
			{
				// CORNER turn: aim the wishdir toward the goal bearing by up to
				// corner_aim deg off velocity. This rotates the VELOCITY vector hard
				// (real cornering) at the cost of some |v| gain -- vs cs->0 which gains
				// max speed but barely turns. Capped so cs never collapses to a near-
				// stall redirect; sign already points at the goal (or open side if a
				// wall flipped it, in which case hard_corner was cleared above).
				corner_mag = (signed_to_goal >= 0.0f) ? signed_to_goal : -signed_to_goal;
				if (corner_mag > corner_aim) corner_mag = corner_aim;
				rotation = corner_mag;
			}
			else if (hor_speed_sq > numerator * numerator)
			{
				// cs->0 accel turn magnitude (max |v| gain), bootstrap until fast.
				rotation = acos(numerator / sqrt(hor_speed_sq)) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}

			RotatePointAroundVector(proposed_dir, up_vector, cur_dir, rotation * sign);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0) { VectorCopy(cur_dir, proposed_dir); }
		}

		// Heading error to the target (|signed_to_goal|); recomputed here so the jump
		// decision sees a sharp bend even when the air-weave branch above didn't run.
		herr = goal_yaw - vel_yaw;
		while (herr > 180.0f) herr -= 360.0f;
		while (herr < -180.0f) herr += 360.0f;
		if (herr < 0) herr = -herr;

		if (gate >= 1 && !moveprobe_s21_teleported[slot] && wp == gate)
		{
			// Stay grounded to enter the floor teleporter (don't hop over the trigger).
			press_jump = false;
		}
		else if (herr > turn_thresh || home_straight
				 || (descend && self->s.v.origin[2] > -10.0f && self->s.v.origin[0] > 1505.0f))
		{
			// x>1505 on the descend branch: keep BUNNYHOPPING through the launch edge
			// (~1477) so the bot flies off at its ~489 qu/s run-up speed instead of
			// ground-braking to ~327 (descend's straight-aim + ground accel snapped the
			// velocity at the nook = a brake; trace 20260609T172340Z showed gating the
			// whole latch instead broke navigation). descend's AIM still steers toward
			// the nook the whole time; only the de-hop waits until past the edge.
			// SHARP BEND (brief), FINAL SETTLE (last 130 qu), or PIT DESCENT ONCE UP ON THE
			// LEDGE (z>-10): stop bunnyhopping so GROUND accel can redirect velocity hard --
			// this is how the human rounds the SE->north hairpin (while landing) and comes
			// to rest in the low RL nook. Airborne at ~470 qu/s the cs->0 accel barely
			// rotates velocity, so the bot overshoots; a brief ground contact restores turn
			// authority. DECOUPLED from descend on the LOWER floor (z<=-10): there the
			// descend latch still aims at RL (turns the bot north at the hairpin) but we
			// KEEP bunnyhopping so the bot can hop UP the ledge step -- grounding it on the
			// z=-24 floor trapped it below the step. The herr>turn_thresh branch still
			// grounds it for the actual turn, so the cornering aid is preserved.
			press_jump = false;
		}
		else
		{
			press_jump = onground && !moveprobe_accel_jump_press[slot];
		}
		moveprobe_accel_jump_press[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;

		return true;
	}

	if (replay_variant == 4)
	{
		// Mode 22: HUMAN-FAITHFUL actuation. Reuses mode 21's solved navigation (the
		// "where to go" -> goal_yaw), but replaces mode 21's forward-only view SNAP with
		// the human technique: a SMOOTH, rate-limited simulated view yaw (the "mouse")
		// co-rotated with a HELD strafe key. That holds the wishdir ~perpendicular to
		// velocity (near-max air accel ~cs 87 deg) THROUGH turns -- the one thing
		// forward-only+snap cannot do (it must trade accel for turning). The strafe sign
		// flips only at genuine turn reversals (~1% rate, not a per-frame buzz). Mirrors
		// the seeded/rate-limited/held-strafe idioms already proven in mode 18 (cstrafe).
		float advance_r = cvar("k_fb_moveprobe_s21_advance");
		float pp_xtrack = cvar("k_fb_moveprobe_s21_pp_xtrack");
		float pp_start = cvar("k_fb_moveprobe_s21_pp_start");
		float turn_thresh = cvar("k_fb_moveprobe_s21_turn");
		float sv_maxspeed = cvar("sv_maxspeed");
		float turnrate = cvar("k_fb_moveprobe_s22_turnrate");
		float lead_gain = cvar("k_fb_moveprobe_s22_lead_gain");
		float lead_max = cvar("k_fb_moveprobe_s22_lead_max");
		float flip_deadband = cvar("k_fb_moveprobe_s22_flip_deadband");
		float flip_min_interval = cvar("k_fb_moveprobe_s22_flip_min_int");
		float fwd_frac = cvar("k_fb_moveprobe_s22_fwd");
		float bootstrap_speed = cvar("k_fb_moveprobe_s22_bootstrap_speed");
		int flip_guard = (int)cvar("k_fb_moveprobe_s22_strafe_flip");
		int wp, scan, i, best, gate, near_idx, carrot, look_fr;
		int want, s;
		float hor_speed_sq, goal_yaw, vel_yaw, bestd, dd, to_goal, crosstrack;
		float heading_err, herr, lead, sim_target, dyaw, ft;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump, home_straight, final_leg, descend, at_gate, precision, fast_air;
		vec3_t to_wp, moved, d, cur_dir;

		if (advance_r <= 0) advance_r = 110.0f;
		if (pp_xtrack <= 0) pp_xtrack = 150.0f;
		if (pp_start <= 0) pp_start = 0.52f;
		if (turn_thresh <= 0) turn_thresh = 35.0f;
		if (sv_maxspeed <= 0) sv_maxspeed = 320.0f;
		if (turnrate <= 0) turnrate = 450.0f;          // human p99 deg/s; the slew clamp = the "mouse"
		if (lead_gain <= 0) lead_gain = 0.5f;
		if (lead_max <= 0) lead_max = 25.0f;
		if (flip_deadband <= 0) flip_deadband = 20.0f;
		if (flip_min_interval <= 0) flip_min_interval = 0.6f;
		if (fwd_frac < 0) fwd_frac = 0.0f;             // 0 is valid (pure strafe)
		if (bootstrap_speed <= 0) bootstrap_speed = 320.0f;
		if (flip_guard < 0) flip_guard = 0;            // 0 is valid (no sign inversion)

		ft = g_globalvars.frametime;

		// ================= NAV: keep in sync with mode 21 (variant 3, L1356-1513) =======
		wp = moveprobe_s21_wp[slot];
		if (wp < 1) wp = 1;

		// Teleporter gate: last frame before the path's big origin jump (the entrance).
		gate = -1;
		for (i = 1; i < count; i++)
		{
			VectorSubtract(frames[i].origin,
						   frames[i - 1].origin, d);
			if (DotProduct(d, d) > 200.0f * 200.0f) { gate = i - 1; break; }
		}

		// Teleport detect: a big one-frame origin jump -> mark passed, resync the cursor.
		VectorSubtract(self->s.v.origin, moveprobe_s21_prevorg[slot], moved);
		if (moveprobe_s21_has_prev[slot] && VectorLength(moved) > 200.0f)
		{
			moveprobe_s21_teleported[slot] = 1;
			best = wp;
			bestd = 1e18f;
			for (i = wp; i < count; i++)
			{
				VectorSubtract(frames[i].origin, self->s.v.origin, d);
				d[2] = 0;
				dd = d[0] * d[0] + d[1] * d[1];
				if (dd < bestd) { bestd = dd; best = i; }
			}
			wp = best + 1;
			if (wp > count - 1) wp = count - 1;
			// Pin the post-teleport curl deterministically (the mode-21 variance fix,
			// applied here to the HELD strafe sign instead of the weave sign).
			moveprobe_s22_strafe_sign[slot] = -1;
			moveprobe_s22_flip_t[slot] = g_globalvars.time;
		}
		VectorCopy(self->s.v.origin, moveprobe_s21_prevorg[slot]);
		moveprobe_s21_has_prev[slot] = 1;

		// Advance the target waypoint by horizontal proximity.
		scan = 0;
		while (wp < count - 1 && scan < 512)
		{
			VectorSubtract(frames[wp].origin, self->s.v.origin, to_wp);
			to_wp[2] = 0;
			if (to_wp[0] * to_wp[0] + to_wp[1] * to_wp[1] <= advance_r * advance_r)
			{
				wp++;
				scan++;
			}
			else
			{
				break;
			}
		}

		// Hold the cursor at the teleporter entrance until the bot has teleported.
		if (gate >= 1 && !moveprobe_s21_teleported[slot] && wp > gate) wp = gate;
		moveprobe_s21_wp[slot] = wp;

		final_leg = (moveprobe_s21_teleported[slot] && wp > (int)(pp_start * count)) ? true : false;

		// Pit-descent latch (position-based, sticky); doubles as the northward-turn aid.
		if (moveprobe_s21_teleported[slot]
			&& self->s.v.origin[0] > 1455.0f
			&& self->s.v.origin[1] > -90.0f
			&& self->s.v.origin[2] > -35.0f)
			moveprobe_s21_descend_latch[slot] = 1;
		descend = moveprobe_s21_descend_latch[slot] ? true : false;

		near_idx = wp;
		if (final_leg)
		{
			// Search a window straddling the cursor for the nearest human path point.
			bestd = 1e18f;
			best = (wp - 24 < 1) ? 1 : wp - 24;
			for (i = best; i < count && i < wp + 128; i++)
			{
				VectorSubtract(frames[i].origin, self->s.v.origin, d);
				d[2] = 0;
				dd = d[0] * d[0] + d[1] * d[1];
				if (dd < bestd) { bestd = dd; near_idx = i; }
			}
			crosstrack = sqrt(bestd);
			// Cross-track-adaptive look-ahead: far off the line -> aim almost AT the
			// nearest point (perpendicular convergence); on the line -> look further ahead.
			look_fr = (crosstrack > pp_xtrack) ? 8 : 16;
			carrot = near_idx + look_fr;
			if (carrot > count - 1) carrot = count - 1;
			if (descend) carrot = count - 1;
			wp = carrot;
			moveprobe_s21_wp[slot] = wp;
		}

		// Heading to the target waypoint.
		VectorSubtract(frames[wp].origin, self->s.v.origin, to_wp);
		to_wp[2] = 0;
		if (VectorNormalize(to_wp) <= 0) { to_wp[0] = 1; to_wp[1] = 0; to_wp[2] = 0; }
		goal_yaw = vectoyaw(to_wp);

		// Current velocity heading.
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];
		if (VectorNormalize(cur_dir) <= 0) { VectorCopy(to_wp, cur_dir); }
		vel_yaw = vectoyaw(cur_dir);

		// Horizontal distance to the FINAL waypoint (RL) -> home-stretch behaviour.
		VectorSubtract(frames[count - 1].origin, self->s.v.origin, d);
		d[2] = 0;
		to_goal = sqrt(d[0] * d[0] + d[1] * d[1]);
		home_straight = (to_goal < 256.0f) ? true : false;
		// ================= END NAV (keep in sync with mode 21) ==========================

		// Signed heading error (CCW positive): how far velocity must rotate to face goal.
		heading_err = goal_yaw - vel_yaw;
		while (heading_err > 180.0f) heading_err -= 360.0f;
		while (heading_err < -180.0f) heading_err += 360.0f;
		herr = (heading_err < 0.0f) ? -heading_err : heading_err;

		at_gate = (gate >= 1 && !moveprobe_s21_teleported[slot] && wp == gate) ? true : false;
		// Precision / redirect states aim the view straight at the goal: the teleporter
		// trigger, the RL-nook settle, the pit descent, OR any grounded frame -- on the
		// ground/at the hairpin, ground accel redirects velocity toward the goal (mode
		// 21's validated cornering aid), which a "fly straight along velocity" rule loses.
		precision = (home_straight || descend || at_gate || onground) ? true : false;
		fast_air = (!precision && hor_speed_sq >= bootstrap_speed * bootstrap_speed) ? true : false;

		// Seed the simulated view lazily from the velocity heading (mirrors mode 18 L2398).
		if (!moveprobe_s22_init[slot])
		{
			if (hor_speed_sq > 1.0f) moveprobe_s22_sim_yaw[slot] = vel_yaw;
			else                     moveprobe_s22_sim_yaw[slot] = self->fb.desired_angle[YAW];
			moveprobe_s22_strafe_sign[slot] = (heading_err >= 0.0f) ? -1 : 1;
			moveprobe_s22_flip_t[slot] = g_globalvars.time;
			moveprobe_s22_init[slot] = 1;
		}

		// Held strafe sign = the side that curls velocity toward the goal. CONVENTION
		// (confirmed by mode 18 L2433: side- turns the view CCW / yaw up): a CCW turn
		// (goal to the LEFT, heading_err > 0) is driven by strafing LEFT (-1). Flip only
		// at genuine reversals (past a deadband, with a minimum interval) so the sign-flip
		// rate is ~1% (continuous arcs) rather than mode 21's per-frame weave buzz.
		want = (heading_err > 0.0f) ? -1 : 1;
		if (flip_guard) want = -want;
		s = moveprobe_s22_strafe_sign[slot];
		if (want != s && herr > flip_deadband
			&& (g_globalvars.time - moveprobe_s22_flip_t[slot]) > flip_min_interval)
		{
			s = want;
			moveprobe_s22_strafe_sign[slot] = s;
			moveprobe_s22_flip_t[slot] = g_globalvars.time;
		}

		// View target: fast air anchors on the velocity heading and LEADS toward the goal
		// (so the held strafe stays ~perpendicular = near-max accel while velocity still
		// rotates); every other state aims straight at the goal.
		if (fast_air)
		{
			lead = lead_gain * heading_err;
			if (lead > lead_max) lead = lead_max;
			if (lead < -lead_max) lead = -lead_max;
			sim_target = vel_yaw + lead;
		}
		else
		{
			sim_target = goal_yaw;
		}

		// Rate-limited smooth slew toward the target -- THIS single clamp IS the "mouse".
		dyaw = sim_target - moveprobe_s22_sim_yaw[slot];
		while (dyaw > 180.0f) dyaw -= 360.0f;
		while (dyaw < -180.0f) dyaw += 360.0f;
		if (dyaw > turnrate * ft) dyaw = turnrate * ft;
		if (dyaw < -turnrate * ft) dyaw = -turnrate * ft;
		moveprobe_s22_sim_yaw[slot] = anglemod(moveprobe_s22_sim_yaw[slot] + dyaw);

		// Jump: duplicated from mode 21 (L1635-1659) -- same gate/corner/home/descend
		// gating and the shared bhop toggle.
		if (gate >= 1 && !moveprobe_s21_teleported[slot] && wp == gate)
		{
			press_jump = false;                       // stay grounded to enter the teleporter
		}
		else if (herr > turn_thresh || home_straight
				 || (descend && self->s.v.origin[2] > -10.0f))
		{
			press_jump = false;                       // sharp bend / final settle / ledge descent
		}
		else
		{
			press_jump = onground && !moveprobe_accel_jump_press[slot];
		}
		moveprobe_accel_jump_press[slot] = press_jump;

		// Emit (mirrors mode 18 L2447-2453): look along the simulated view; hold the
		// strafe key. Fast air = pure/held strafe. Bootstrap (slow air) = forward + strafe
		// to build |v| fast. Precision/ground = straight forward, no strafe (a held strafe
		// cannot point at and hit a small floor trigger / settle in the nook).
		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = moveprobe_s22_sim_yaw[slot];
		self->fb.desired_angle[ROLL] = 0;
		if (fast_air)
		{
			direction[0] = fwd_frac * sv_maxspeed;    // 0 = pure strafe
			direction[1] = s * sv_maxspeed;           // held strafe key (+ = right)
		}
		else if (!precision)
		{
			direction[0] = sv_maxspeed;               // bootstrap: forward + strafe accel
			direction[1] = s * sv_maxspeed;
		}
		else
		{
			direction[0] = sv_maxspeed;               // precision / ground: straight forward
			direction[1] = 0;
		}
		direction[2] = 0;
		*jumping = press_jump;

		return true;
	}

	if (replay_variant == 5)
	{
		// Mode 25: human-mouse speed catch-up. Keep the recorded view angles
		// as the mouse trace and keep the replay clock/divergence scoring, but
		// when live horizontal speed falls materially behind the human frame,
		// replace only the movement vector with a velocity-relative air-strafe
		// wishdir. This tests whether the bot can beat the speed envelope while
		// preserving the human mouse timing as the first-class signal.
		float hs = sqrt(self->s.v.velocity[0] * self->s.v.velocity[0]
						+ self->s.v.velocity[1] * self->s.v.velocity[1]);
		float target_hs = sqrt(f->velocity[0] * f->velocity[0]
								+ f->velocity[1] * f->velocity[1]);
		float gap = cvar("k_fb_moveprobe_s25_gap");
		float min_speed = cvar("k_fb_moveprobe_s25_min_speed");
		float numerator = cvar("k_fb_moveprobe_s25_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_s25_bootstrap_deg");
		float move = cvar("k_fb_moveprobe_s25_move");
		float path_div = cvar("k_fb_moveprobe_s25_path_div");
		float path_blend = cvar("k_fb_moveprobe_s25_path_blend");
		float phase_start = cvar("k_fb_moveprobe_s25_phase_start");
		float phase_target = cvar("k_fb_moveprobe_s25_phase_target");
		float phase_min_speed = cvar("k_fb_moveprobe_s25_phase_min_speed");
		float phase_move = cvar("k_fb_moveprobe_s25_phase_move");
		float phase_numerator = cvar("k_fb_moveprobe_s25_phase_numerator");
		float phase_human_cmd = cvar("k_fb_moveprobe_s25_phase_human_cmd");
		float phase2_start = cvar("k_fb_moveprobe_s25_phase2_start");
		float phase2_move = cvar("k_fb_moveprobe_s25_phase2_move");
		float phase_jump = cvar("k_fb_moveprobe_s25_phase_jump");
		float phase_gap_gain = cvar("k_fb_moveprobe_s25_phase_gap_gain");
		float phase_move_max = cvar("k_fb_moveprobe_s25_phase_move_max");
		float phase_yaw_offset = cvar("k_fb_moveprobe_s25_phase_yaw_offset");
		float phase_human_scale = cvar("k_fb_moveprobe_s25_phase_human_scale");
		float phase_lane_nudge = cvar("k_fb_moveprobe_s25_phase_lane_nudge");
		float phase_lane_deadband = cvar("k_fb_moveprobe_s25_phase_lane_deadband");
		float phase_lane_max = cvar("k_fb_moveprobe_s25_phase_lane_max");
		int flip = (int)cvar("k_fb_moveprobe_s25_flip");
		int vel_sign = (int)cvar("k_fb_moveprobe_s25_velsign");
		int sign = 0;
		vec3_t cur_dir;
		vec3_t wish_dir;
		vec3_t up_vector = { 0, 0, 1 };
		vec3_t target_dir;
		qbool speed_reason, path_reason, phase_reason;

		if (gap <= 0) gap = 32.0f;
		if (min_speed <= 0) min_speed = 320.0f;
		if (numerator <= 0) numerator = 8.0f;
		if (bootstrap_deg <= 0) bootstrap_deg = 25.0f;
		if (move <= 0) move = 400.0f;
		if (path_div < 0) path_div = 0.0f;
		path_blend = bound(0.0f, path_blend, 1.0f);
		if (phase_start < 0) phase_start = 0.0f;
		if (phase_target <= 0) phase_target = 850.0f;
		if (phase_min_speed < 0) phase_min_speed = 0.0f;
		if (phase_move <= 0) phase_move = move;
		if (phase_numerator <= 0) phase_numerator = numerator;
		phase_human_cmd = bound(0.0f, phase_human_cmd, 1.0f);
		if (phase2_start < 0) phase2_start = 0.0f;
		if (phase2_move <= 0) phase2_move = phase_move;
		phase_jump = bound(0.0f, phase_jump, 1.0f);
		if (phase_gap_gain < 0) phase_gap_gain = 0.0f;
		if (phase_move_max < 0) phase_move_max = 0.0f;
		phase_yaw_offset = bound(-20.0f, phase_yaw_offset, 20.0f);
		if (phase_human_scale < 0) phase_human_scale = 0.0f;
		if (phase_lane_nudge < 0.0f) phase_lane_nudge = 0.0f;
		if (phase_lane_deadband <= 0.0f) phase_lane_deadband = 6.0f;
		if (phase_lane_max <= 0.0f) phase_lane_max = 96.0f;

		VectorCopy(f->angles, self->fb.desired_angle);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = f->forwardmove;
		direction[1] = f->sidemove;
		direction[2] = f->upmove;
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		target_dir[0] = f->velocity[0];
		target_dir[1] = f->velocity[1];
		target_dir[2] = 0;

		moveprobe_s25_active[slot] = 1;
		moveprobe_s25_engaged[slot] = 0;
		moveprobe_s25_reason[slot] = 0;
		moveprobe_s25_sign[slot] = 0;
		moveprobe_s25_hs[slot] = hs;
		moveprobe_s25_target_hs[slot] = target_hs;
		moveprobe_s25_speed_gap[slot] = target_hs - hs;
		moveprobe_s25_rotation[slot] = 0.0f;
		moveprobe_s25_wish_yaw[slot] = 0.0f;
		moveprobe_s25_vel_yaw[slot] = 0.0f;
		moveprobe_s25_target_vel_yaw[slot] = 0.0f;
		moveprobe_s25_target_vel_err[slot] = 0.0f;
		moveprobe_s25_out_fwd[slot] = direction[0];
		moveprobe_s25_out_side[slot] = direction[1];
		if (VectorNormalize(cur_dir) > 0)
		{
			moveprobe_s25_vel_yaw[slot] = anglemod(vectoyaw(cur_dir));
		}
		if (VectorNormalize(target_dir) > 0)
		{
			moveprobe_s25_target_vel_yaw[slot] = anglemod(vectoyaw(target_dir));
			moveprobe_s25_target_vel_err[slot] =
				BotMoveProbeWrap180(moveprobe_s25_target_vel_yaw[slot]
									 - moveprobe_s25_vel_yaw[slot]);
		}

		speed_reason = (target_hs > hs + gap) ? true : false;
		path_reason = ((path_div > 0.0f) && (path_blend > 0.0f)
			&& (moveprobe_replay_divergence_h[slot] > path_div)) ? true : false;
		phase_reason = ((phase_start > 0.0f) && (cursor >= (int)phase_start)
			&& (target_hs >= phase_target) && (hs >= phase_min_speed)) ? true : false;
		if (phase_reason && (phase_yaw_offset != 0.0f))
		{
			self->fb.desired_angle[YAW] =
				anglemod(self->fb.desired_angle[YAW] + phase_yaw_offset);
			moveprobe_s25_reason[slot] |= 512;
		}
		if (((hs >= min_speed) && (speed_reason || path_reason)) || phase_reason)
		{
			cur_dir[0] = self->s.v.velocity[0];
			cur_dir[1] = self->s.v.velocity[1];
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) > 0)
			{
				float rotation;
				float effective_move = phase_reason ? phase_move : move;
				float effective_numerator = phase_reason ? phase_numerator : numerator;
				vec3_t view_ang;

				if (phase_reason && (phase2_start > 0.0f)
					&& (cursor >= (int)phase2_start))
				{
					effective_move = phase2_move;
					moveprobe_s25_reason[slot] |= 64;
				}
				if (phase_reason && (phase_gap_gain > 0.0f)
					&& (target_hs > hs))
				{
					effective_move += (target_hs - hs) * phase_gap_gain;
					if (phase_move_max > 0.0f)
					{
						effective_move = min(effective_move, phase_move_max);
					}
					moveprobe_s25_reason[slot] |= 256;
				}
				if (f->sidemove < 0) sign = 1;
				else if (f->sidemove > 0) sign = -1;
				else if (cursor > 0)
				{
					float dyaw = f->angles[YAW] - frames[cursor - 1].angles[YAW];
					while (dyaw > 180.0f) dyaw -= 360.0f;
					while (dyaw < -180.0f) dyaw += 360.0f;
					if (dyaw > 0.01f) sign = 1;
					else if (dyaw < -0.01f) sign = -1;
				}
				if (sign == 0)
				{
					sign = moveprobe_accel_strafe_sign[slot];
				}
				if (sign == 0)
				{
					sign = 1;
				}
				if (hs > effective_numerator)
				{
					rotation = acos(effective_numerator / hs) * 180.0f / M_PI;
				}
				else
				{
					rotation = bootstrap_deg;
				}
				if (speed_reason)
				{
					moveprobe_s25_reason[slot] |= 1;
				}
				if (path_reason)
				{
					moveprobe_s25_reason[slot] |= 2;
				}
				if (phase_reason)
				{
					moveprobe_s25_reason[slot] |= 16;
				}
				if (vel_sign && (target_hs > 1.0f))
				{
					vec3_t plus_dir;
					vec3_t minus_dir;

					target_dir[0] = f->velocity[0];
					target_dir[1] = f->velocity[1];
					target_dir[2] = 0;
					if (VectorNormalize(target_dir) > 0)
					{
						RotatePointAroundVector(plus_dir, up_vector, cur_dir, rotation);
						RotatePointAroundVector(minus_dir, up_vector, cur_dir, -rotation);
						plus_dir[2] = 0;
						minus_dir[2] = 0;
						if ((VectorNormalize(plus_dir) > 0)
							&& (VectorNormalize(minus_dir) > 0))
						{
							sign = (DotProduct(plus_dir, target_dir)
								>= DotProduct(minus_dir, target_dir)) ? 1 : -1;
						}
					}
					moveprobe_s25_reason[slot] |= 4;
				}
				if (flip)
				{
					sign = -sign;
				}
				moveprobe_accel_strafe_sign[slot] = sign;
				RotatePointAroundVector(wish_dir, up_vector, cur_dir, rotation * sign);
				wish_dir[2] = 0;
				if (VectorNormalize(wish_dir) > 0)
				{
					if (path_reason)
					{
						vec3_t to_human;

						VectorSubtract(f->origin, self->s.v.origin, to_human);
						to_human[2] = 0;
						if (VectorNormalize(to_human) > 0)
						{
							wish_dir[0] = wish_dir[0] * (1.0f - path_blend)
								+ to_human[0] * path_blend;
							wish_dir[1] = wish_dir[1] * (1.0f - path_blend)
								+ to_human[1] * path_blend;
							wish_dir[2] = 0;
							if (VectorNormalize(wish_dir) <= 0)
							{
								VectorCopy(to_human, wish_dir);
							}
							moveprobe_s25_reason[slot] |= 8;
						}
					}
					if (phase_reason && (phase_human_scale > 0.0f))
					{
						direction[0] = f->forwardmove * phase_human_scale;
						direction[1] = f->sidemove * phase_human_scale;
						moveprobe_s25_reason[slot] |= 1024;
					}
					else if (phase_reason && (phase_human_cmd > 0.0f))
					{
						float human_side = 0.0f;

						if (f->sidemove < 0) human_side = -1.0f;
						else if (f->sidemove > 0) human_side = 1.0f;
						else if (sign) human_side = -sign;
						direction[0] = f->forwardmove;
						direction[1] = human_side * effective_move;
						moveprobe_s25_reason[slot] |= 32;
						if (phase_lane_nudge > 0.0f)
						{
							vec3_t to_human;
							float lane_dist;

							VectorSubtract(f->origin, self->s.v.origin, to_human);
							to_human[2] = 0;
							lane_dist = VectorNormalize(to_human);
							if (lane_dist > phase_lane_deadband)
							{
								float lane_move = (lane_dist - phase_lane_deadband) * phase_lane_nudge;

								if (phase_lane_max > 0.0f)
								{
									lane_move = min(lane_move, phase_lane_max);
								}
								view_ang[0] = f->angles[PITCH];
								view_ang[1] = f->angles[YAW];
								view_ang[2] = 0;
								trap_makevectors(view_ang);
								direction[0] += DotProduct(g_globalvars.v_forward, to_human) * lane_move;
								direction[1] += DotProduct(g_globalvars.v_right, to_human) * lane_move;
								moveprobe_s25_reason[slot] |= 2048;
							}
						}
					}
					else
					{
						view_ang[0] = f->angles[PITCH];
						view_ang[1] = f->angles[YAW];
						view_ang[2] = 0;
						trap_makevectors(view_ang);
						direction[0] = DotProduct(g_globalvars.v_forward, wish_dir) * effective_move;
						direction[1] = DotProduct(g_globalvars.v_right, wish_dir) * effective_move;
					}
					moveprobe_s25_engaged[slot] = 1;
					moveprobe_s25_sign[slot] = sign;
					moveprobe_s25_rotation[slot] = rotation;
					moveprobe_s25_wish_yaw[slot] = anglemod(vectoyaw(wish_dir));
					moveprobe_s25_out_fwd[slot] = direction[0];
					moveprobe_s25_out_side[slot] = direction[1];
				}
			}
		}

		if (phase_reason && (phase_jump > 0.0f))
		{
			*jumping = true;
			moveprobe_s25_reason[slot] |= 128;
		}
		else
		{
			*jumping = (f->buttons & 2) ? true : false;
		}
		return true;
	}

	// Mode 12: corrective replay. Emit the EXACT human usercmd (preserving the
	// proven open-loop lockstep prefix), but once horizontal divergence exceeds a
	// deadband, rotate the view yaw toward the human origin by a small clamped
	// budget. Same input magnitudes as the human -- only the heading is corrected.
	// A yaw nudge never writes origin/velocity, so the divergence trace stays a
	// valid (non-self-zeroing) signal, unlike a position or velocity pull.
	{
		float deadband = cvar("k_fb_moveprobe_corr_deadband");
		float yaw_max = cvar("k_fb_moveprobe_corr_yaw_max");
		float applied = 0.0f;
		qbool realigning = false;

		if (deadband <= 0)
		{
			deadband = 16.0f;
		}
		if (yaw_max <= 0)
		{
			yaw_max = 3.0f;
		}
		yaw_max = bound(0.1f, yaw_max, 20.0f);

		VectorCopy(f->angles, self->fb.desired_angle);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = f->forwardmove;
		direction[1] = f->sidemove;
		direction[2] = f->upmove;
		*jumping = (f->buttons & 2) ? true : false;
		if (cvar("k_fb_moveprobe_corr_cmd_blend") > 0.0f)
		{
			float cmd_blend = bound(0.0f, cvar("k_fb_moveprobe_corr_cmd_blend"), 1.0f);
			float cmd_deadband = cvar("k_fb_moveprobe_corr_cmd_deadband");
			float cmd_move = cvar("k_fb_moveprobe_corr_cmd_move");
			int cmd_after = (int)cvar("k_fb_moveprobe_corr_cmd_after");
			float recorded_move = sqrt(f->forwardmove * f->forwardmove + f->sidemove * f->sidemove);

			if (cmd_deadband <= 0.0f)
			{
				cmd_deadband = 8.0f;
			}
			if (cmd_after < 0)
			{
				cmd_after = 0;
			}
			if (cmd_move <= 0.0f)
			{
				cmd_move = recorded_move;
			}
			if ((recorded_move > 0.0f)
				&& (cmd_move > 0.0f)
				&& (cursor >= cmd_after)
				&& (moveprobe_replay_divergence_h[slot] > cmd_deadband))
			{
				vec3_t view_ang, recorded_wish, to_human, wish_dir;

				view_ang[0] = f->angles[PITCH];
				view_ang[1] = f->angles[YAW];
				view_ang[2] = 0;
				trap_makevectors(view_ang);
				recorded_wish[0] = g_globalvars.v_forward[0] * f->forwardmove
					+ g_globalvars.v_right[0] * f->sidemove;
				recorded_wish[1] = g_globalvars.v_forward[1] * f->forwardmove
					+ g_globalvars.v_right[1] * f->sidemove;
				recorded_wish[2] = 0;
				VectorSubtract(f->origin, self->s.v.origin, to_human);
				to_human[2] = 0;
				if ((VectorNormalize(recorded_wish) > 0) && (VectorNormalize(to_human) > 0))
				{
					wish_dir[0] = recorded_wish[0] * (1.0f - cmd_blend)
						+ to_human[0] * cmd_blend;
					wish_dir[1] = recorded_wish[1] * (1.0f - cmd_blend)
						+ to_human[1] * cmd_blend;
					wish_dir[2] = 0;
					if (VectorNormalize(wish_dir) <= 0)
					{
						VectorCopy(recorded_wish, wish_dir);
					}
					direction[0] = DotProduct(g_globalvars.v_forward, wish_dir) * cmd_move;
					direction[1] = DotProduct(g_globalvars.v_right, wish_dir) * cmd_move;
					direction[2] = f->upmove;
				}
			}
		}
		if ((cvar("k_fb_moveprobe_corr_ground_realign") > 0.0f)
			&& ((int)self->s.v.flags & FL_ONGROUND)
			&& (f->forwardmove == 0)
			&& (f->sidemove == 0)
			&& (f->upmove == 0)
			&& !(f->buttons & 3))
		{
			float align_deadband = cvar("k_fb_moveprobe_corr_ground_realign_deadband");
			float align_move = cvar("k_fb_moveprobe_corr_ground_realign_move");
			int align_after = (int)cvar("k_fb_moveprobe_corr_ground_realign_after");

			if (align_deadband <= 0.0f)
			{
				align_deadband = 12.0f;
			}
			if (align_move <= 0.0f)
			{
				align_move = 320.0f;
			}
			if (align_after < 0)
			{
				align_after = 0;
			}
			if ((cursor >= align_after) && (moveprobe_replay_divergence_h[slot] > align_deadband))
			{
				vec3_t to_human;

				VectorSubtract(f->origin, self->s.v.origin, to_human);
				to_human[2] = 0;
				if (VectorLength(to_human) > 1.0f)
				{
					float target_yaw = vectoyaw(to_human);

					self->fb.desired_angle[YAW] = anglemod(target_yaw);
					direction[0] = align_move;
					direction[1] = 0;
					direction[2] = 0;
					*jumping = false;
					realigning = true;
				}
			}
		}
		if (!realigning && !*jumping && (cvar("k_fb_moveprobe_corr_autojump") > 0.0f)
			&& ((int)self->s.v.flags & FL_ONGROUND))
		{
			*jumping = true;
		}

		if (moveprobe_replay_divergence_h[slot] > deadband)
		{
			vec3_t to_human;

			VectorSubtract(f->origin, self->s.v.origin, to_human);
			to_human[2] = 0;
			if (VectorLength(to_human) > 1.0f)
			{
				float target_yaw = vectoyaw(to_human);
				float cur_yaw = self->fb.desired_angle[YAW];
				float dyaw = anglemod(target_yaw) - anglemod(cur_yaw);

				while (dyaw > 180.0f)
				{
					dyaw -= 360.0f;
				}
				while (dyaw < -180.0f)
				{
					dyaw += 360.0f;
				}
				dyaw = bound(-yaw_max, dyaw, yaw_max);
				self->fb.desired_angle[YAW] = anglemod(cur_yaw + dyaw);
				applied = (dyaw < 0.0f) ? -dyaw : dyaw;
			}
		}

		moveprobe_replay_corr_accum[slot] += applied;
		if (applied > moveprobe_replay_corr_max[slot])
		{
			moveprobe_replay_corr_max[slot] = applied;
		}

		return true;
	}
}

static char moveprobe_assign_signature[MAX_CLIENTS][320];

// Copies a cvar value into a single-token display field: empty -> "-",
// whitespace -> ','.
static void BotMoveProbeAssignField(const char *value, char *out, int out_size)
{
	int i;

	strlcpy(out, value[0] ? value : "-", out_size);
	for (i = 0; out[i]; i++)
	{
		if (out[i] == ' ' || out[i] == '\t')
		{
			out[i] = ',';
		}
	}
}

// LD-F1 (#95): one-time FBMOVEPROBE_ASSIGN row per bot, re-emitted only when
// the resolved assignment changes. It exposes which mode/route/goal/spawn the
// bot is ACTUALLY running (and whether each came from the per-slot or global
// cvar) so telemetry rows join to assignments (consumed by LD-F3).
static void BotMoveProbeEmitAssignRow(gedict_t *self, int slot, int mode, int mode_from_slot)
{
	char replay_raw[128], spawn_raw[64];
	char replay_shown[128], spawn_shown[64];
	char signature[320];
	int replay_from_slot;
	int spawn_from_slot;
	int goal_from_slot = 0;
	int fixed_goal = 0;

	if (!cvar("k_fb_moveprobe_log_commands") || slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	replay_from_slot = BotMoveProbeCvarStringForBot(self, "replay_file", replay_raw,
													sizeof(replay_raw));
	spawn_from_slot = BotMoveProbeCvarStringForBot(self, "spawn_origin", spawn_raw,
												   sizeof(spawn_raw));
	if (BotMoveProbeCvarIntForBot(self, "fixed_goal", &fixed_goal, &goal_from_slot) < 0)
	{
		return; // malformed pin: the loud-fail path owns this slot's output
	}
	BotMoveProbeAssignField(replay_raw, replay_shown, sizeof(replay_shown));
	BotMoveProbeAssignField(spawn_raw, spawn_shown, sizeof(spawn_shown));

	snprintf(signature, sizeof(signature), "%d|%d|%s|%d|%d|%d|%s|%d",
			 mode, mode_from_slot, replay_shown, replay_from_slot,
			 fixed_goal, goal_from_slot, spawn_shown, spawn_from_slot);
	if (streq(signature, moveprobe_assign_signature[slot]))
	{
		return;
	}
	strlcpy(moveprobe_assign_signature[slot], signature,
			sizeof(moveprobe_assign_signature[slot]));

	G_cprint("FBMOVEPROBE_ASSIGN time=%.3f ed=%d name=%s mode=%d mode_src=%s "
			 "replay_file=%s replay_src=%s fixed_goal=%d goal_src=%s "
			 "spawn_origin=%s spawn_src=%s\n",
			 g_globalvars.time, NUM_FOR_EDICT(self), self->netname,
			 mode, mode_from_slot ? "slot" : "global",
			 replay_shown, replay_from_slot ? "slot" : "global",
			 fixed_goal, goal_from_slot ? "slot" : "global",
			 spawn_shown, spawn_from_slot ? "slot" : "global");
}

static void BotApplyMoveProbe(gedict_t *self, qbool *jumping, qbool *firing, int *impulse, vec3_t direction)
{
	int mode = 0;
	int mode_from_slot = 0;
	int perslot_hold = 0;
	int replay_backed_mode = 0;
	int slot = NUM_FOR_EDICT(self) - 1;

	if ((slot >= 0) && (slot < MAX_CLIENTS))
	{
		moveprobe_replay_cmd_msec[slot] = 0;
	}

	// Per-slot resolution (#95): k_fb_moveprobe_mode_s<N> overrides the global
	// mode for this bot; malformed values hold the bot (loud-fail contract).
	if (BotMoveProbeCvarIntForBot(self, "mode", &mode, &mode_from_slot) < 0)
	{
		perslot_hold = 1;
		mode = 0;
	}
	if ((slot >= 0) && (slot < MAX_CLIENTS) && fb_moveprobe_perslot_goal_error[slot])
	{
		perslot_hold = 1;
	}
	replay_backed_mode = (mode == 10 || mode == 11 || mode == 12 || mode == 21 || mode == 22 || mode == 25 || mode == 27);

	BotMoveProbeResetTransition(slot);
	if (mode != 9 && mode != 26 && mode != 27 && mode != 28)
	{
		BotMoveProbeResetQwdSession(slot);
	}
	if (!replay_backed_mode)
	{
		BotMoveProbeResetReplaySession(slot);
	}

	if (perslot_hold)
	{
		// Loud-fail contract (#95): a malformed per-slot assignment holds the
		// bot at spawn rather than running a wrong/unintended configuration.
		// The FBMOVEPROBE_PERSLOT_ERROR row was already printed (throttled).
		BotMoveProbeResetTransitionTiming(slot);
		BotMoveProbeResetQwdSession(slot);
		BotMoveProbeResetReplaySession(slot);
		if ((slot >= 0) && (slot < MAX_CLIENTS))
		{
			moveprobe_assign_signature[slot][0] = '\0';
		}
		VectorClear(direction);
		*jumping = false;
		return;
	}

	if ((mode <= 0) || intermission_running || ISDEAD(self)
		|| ((match_in_progress != 2) && cvar(FB_CVAR_FREEZE_PREWAR)))
	{
		BotMoveProbeResetTransitionTiming(slot);
		BotMoveProbeResetQwdSession(slot);
		if (ISDEAD(self) && replay_backed_mode && BotMoveProbeReplayOneShot()
			&& (slot >= 0) && (slot < MAX_CLIENTS)
			&& (moveprobe_replay_attempt_num[slot] > 0))
		{
			moveprobe_replay_complete[slot] = 1;
			moveprobe_replay_active[slot] = 0;
		}
		else
		{
			BotMoveProbeResetReplaySession(slot);
		}
		if ((slot >= 0) && (slot < MAX_CLIENTS))
		{
			moveprobe_assign_signature[slot][0] = '\0';
		}
		return;
	}

	if (mode != 8)
	{
		BotMoveProbeResetTransitionTiming(slot);
	}

	// Lab instrument: one-time spawn snap. If k_fb_moveprobe_spawn_origin is set
	// ("x y z"), teleport the bot there on its first moveprobe frame (per slot).
	// Optional k_fb_moveprobe_spawn_velocity ("vx vy vz") seeds the matching
	// live velocity for reference states that already carry momentum.
	{
		static int moveprobe_spawn_snapped[MAX_CLIENTS];
		static char moveprobe_spawn_last[MAX_CLIENTS][64];
		static char moveprobe_spawn_velocity_last[MAX_CLIENTS][64];
		static int moveprobe_spawn_last_from_slot[MAX_CLIENTS];
		static int moveprobe_spawn_velocity_last_from_slot[MAX_CLIENTS];
		char snap_buf[64];
		char snap_velocity_buf[64];
		int snap_from_slot, snap_velocity_from_slot;

		snap_from_slot = BotMoveProbeCvarStringForBot(self, "spawn_origin", snap_buf,
													  sizeof(snap_buf));
		snap_velocity_from_slot = BotMoveProbeCvarStringForBot(self, "spawn_velocity",
															   snap_velocity_buf,
															   sizeof(snap_velocity_buf));
		// A changed spawn assignment must re-arm the one-shot snap latch:
		// otherwise a per-slot value edited mid-session (e.g. reassigning a
		// slot without a server restart) is never re-parsed, so a malformed
		// triplet would be neither reported nor held (#95 review). Pure-global
		// configs never take this branch (additive guarantee).
		if ((snap_from_slot || snap_velocity_from_slot
			 || moveprobe_spawn_last_from_slot[slot]
			 || moveprobe_spawn_velocity_last_from_slot[slot])
			&& (!streq(snap_buf, moveprobe_spawn_last[slot])
				|| !streq(snap_velocity_buf, moveprobe_spawn_velocity_last[slot])
				|| (snap_from_slot != moveprobe_spawn_last_from_slot[slot])
				|| (snap_velocity_from_slot != moveprobe_spawn_velocity_last_from_slot[slot])))
		{
			moveprobe_spawn_snapped[slot] = 0;
		}
		strlcpy(moveprobe_spawn_last[slot], snap_buf,
				sizeof(moveprobe_spawn_last[slot]));
		strlcpy(moveprobe_spawn_velocity_last[slot], snap_velocity_buf,
				sizeof(moveprobe_spawn_velocity_last[slot]));
		moveprobe_spawn_last_from_slot[slot] = snap_from_slot;
		moveprobe_spawn_velocity_last_from_slot[slot] = snap_velocity_from_slot;
		if (snap_buf[0])
		{
			if (!moveprobe_spawn_snapped[slot])
			{
				float sx, sy, sz;
				float svx = 0.0f, svy = 0.0f, svz = 0.0f;

				if (sscanf(snap_buf, "%f %f %f", &sx, &sy, &sz) == 3)
				{
					vec3_t snap_org;

					if (snap_velocity_buf[0]
						&& (sscanf(snap_velocity_buf, "%f %f %f", &svx, &svy, &svz) != 3))
					{
						if (snap_velocity_from_slot)
						{
							BotMoveProbeReportPerSlotError(self, "spawn_velocity",
														   snap_velocity_buf,
														   "bad_velocity_triplet");
							VectorClear(direction);
							*jumping = false;
							return;
						}
						svx = svy = svz = 0.0f;
					}
					VectorSet(snap_org, sx, sy, sz);
					setorigin(self, PASSVEC3(snap_org));
					VectorSet(self->s.v.velocity, svx, svy, svz);
				}
				else if (snap_from_slot)
				{
					// Loud-fail (#95): a bad per-slot origin triplet must not
					// run the bot from a wrong start. Do not mark the slot
					// snapped (the error repeats, throttled) and hold.
					BotMoveProbeReportPerSlotError(self, "spawn_origin", snap_buf,
												   "bad_origin_triplet");
					VectorClear(direction);
					*jumping = false;
					return;
				}
				moveprobe_spawn_snapped[slot] = 1;
				// New attempt: re-arm the one-shot circle-jump launch latch
				// (A3 #75) together with the snap itself.
				moveprobe_s23_launch_done[slot] = 0;
				moveprobe_s23_launch_since[slot] = 0;
			}
		}
		else
		{
			moveprobe_spawn_snapped[slot] = 0;
			moveprobe_s23_launch_done[slot] = 0;
			moveprobe_s23_launch_since[slot] = 0;
		}
	}

	BotMoveProbeEmitAssignRow(self, slot, mode, mode_from_slot);

	if (mode == 24)
	{
		// Dashboard practice idle: allow spawn-snap/ASSIGN above, then hold
		// still until a per-slot route assignment overrides this global mode.
		VectorClear(direction);
		*jumping = false;
		*firing = false;
		*impulse = 0;
		return;
	}

	if (mode == 1)
	{
		*jumping = true;

		return;
	}

	else if (mode == 2)
	{
		// Probe-only: string cvar lookups keep this patch small; do not copy
		// this hot-path pattern into a durable movement controller.
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = cvar("k_fb_moveprobe_yaw");
		self->fb.desired_angle[ROLL] = 0;

		direction[0] = forwardmove;
		direction[1] = cvar("k_fb_moveprobe_sidemove");
		direction[2] = cvar("k_fb_moveprobe_upmove");

		*jumping = true;
	}

	else if (mode == 3)
	{
		vec3_t route_direction;
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");

		if (forwardmove <= 0)
		{
			forwardmove = 800;
		}

		VectorCopy(self->fb.dir_move_, route_direction);
		route_direction[2] = 0;
		if (VectorNormalize(route_direction) <= 0)
		{
			return;
		}

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(route_direction);
		self->fb.desired_angle[ROLL] = 0;

		direction[0] = forwardmove;
		direction[1] = cvar("k_fb_moveprobe_sidemove");
		direction[2] = cvar("k_fb_moveprobe_upmove");

		*jumping = true;
	}

	else if (mode == 4)
	{
		vec3_t route_direction;
		int slot = NUM_FOR_EDICT(self) - 1;
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");
		float sidemove = cvar("k_fb_moveprobe_sidemove");
		float strafe_sign = (((int)(g_globalvars.time * 5) + slot) & 1) ? 1 : -1;

		if (forwardmove <= 0)
		{
			forwardmove = 800;
		}
		if (sidemove == 0)
		{
			sidemove = 400;
		}
		if (sidemove < 0)
		{
			sidemove = -sidemove;
		}

		VectorCopy(self->fb.dir_move_, route_direction);
		route_direction[2] = 0;
		if (VectorNormalize(route_direction) <= 0)
		{
			return;
		}

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(route_direction);
		self->fb.desired_angle[ROLL] = 0;

		direction[0] = forwardmove;
		direction[1] = sidemove * strafe_sign;
		direction[2] = cvar("k_fb_moveprobe_upmove");

		*jumping = true;
	}

	else if ((mode == 5) || (mode == 6) || (mode == 7) || (mode == 8) || (mode == 9))
	{
		vec3_t route_direction;
		vec3_t route_right;
		vec3_t desired_move;
		vec3_t move_angles;
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");
		float sidemove = cvar("k_fb_moveprobe_sidemove");
		float native_upmove = direction[2];
		float transition_window = cvar("k_fb_moveprobe_transition_window");
		float transition_scale = cvar("k_fb_moveprobe_transition_scale");
		qbool transition_active = false;
		float strafe_sign = (((int)(g_globalvars.time * 5) + slot) & 1) ? 1 : -1;

		if (forwardmove <= 0)
		{
			forwardmove = 800;
		}
		if (sidemove < 0)
		{
			sidemove = -sidemove;
		}

		if (mode == 9)
		{
			if (forwardmove <= 0)
			{
				forwardmove = 320;
			}
			if (sidemove == 0)
			{
				sidemove = 508;
			}
			if (!BotMoveProbeQwdActive(self, slot, route_direction))
			{
				return;
			}
		}
		else
		{
			VectorCopy(self->fb.dir_move_, route_direction);
			route_direction[2] = 0;
			if (VectorNormalize(route_direction) <= 0)
			{
				return;
			}
		}

		route_right[0] = route_direction[1];
		route_right[1] = -route_direction[0];
		route_right[2] = 0;

		VectorScale(route_direction, forwardmove, desired_move);
		VectorMA(desired_move, sidemove * strafe_sign, route_right, desired_move);

		if (mode == 8)
		{
			if (transition_window <= 0)
			{
				transition_window = 0.4f;
			}
			if (transition_scale <= 0)
			{
				transition_scale = 1.25f;
			}
			transition_window = bound(0.05f, transition_window, 1.0f);
			transition_scale = bound(1.0f, transition_scale, 2.0f);
			transition_active = BotMoveProbeTransitionActive(self, slot, *jumping, transition_window);
			if (transition_active)
			{
				VectorScale(desired_move, transition_scale, desired_move);
				moveprobe_transition_scale_used[slot] = transition_scale;
			}
		}

		VectorCopy(self->fb.desired_angle, move_angles);
		move_angles[PITCH] = 0;
		move_angles[ROLL] = 0;
		// trap_makevectors mutates g_globalvars.v_forward/v_right/v_up; keep
		// this projection immediately adjacent to the dot products below.
		trap_makevectors(move_angles);

		direction[0] = DotProduct(g_globalvars.v_forward, desired_move);
		direction[1] = DotProduct(g_globalvars.v_right, desired_move);
		direction[2] = (((mode == 7) || (mode == 8) || (mode == 9)) && (self->s.v.waterlevel > 1))
			? native_upmove : cvar("k_fb_moveprobe_upmove");

		if (((mode == 6) || (mode == 7) || (mode == 8) || (mode == 9)) && (direction[0] < 0))
		{
			float side_sign = direction[1] < 0 ? -1 : 1;

			if (direction[1] == 0)
			{
				side_sign = strafe_sign;
			}
			direction[1] += -direction[0] * side_sign;
			direction[0] = 0;
		}

		if ((mode == 7) || (mode == 8) || (mode == 9))
		{
			vec3_t command_horizontal;
			float max_horizontal = VectorLength(desired_move);
			float command_horizontal_speed;

			command_horizontal[0] = direction[0];
			command_horizontal[1] = direction[1];
			command_horizontal[2] = 0;
			command_horizontal_speed = VectorLength(command_horizontal);
			if ((max_horizontal > 0) && (command_horizontal_speed > max_horizontal))
			{
				float scale = max_horizontal / command_horizontal_speed;
				direction[0] *= scale;
				direction[1] *= scale;
			}
		}

		*jumping = true;
	}

	else if (mode == 10)
	{
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 0);
	}

	else if (mode == 11)
	{
		// Closed-loop steering: shares the mode-10 replay timeline + divergence,
		// but re-derives the heading toward the human path from the bot's actual
		// origin each frame instead of replaying the exact usercmd. See the
		// k_fb_moveprobe_lookahead_frames cvar and docs/08_DECISION_LOG.md.
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 1);
	}

	else if (mode == 12)
	{
		// Corrective replay: emits the exact human usercmd (preserving the
		// open-loop lockstep prefix) plus a clamped yaw nudge once divergence
		// exceeds k_fb_moveprobe_corr_deadband. See docs/08_DECISION_LOG.md.
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 2);
	}

	else if (mode == 21)
	{
		// Generative route-follow: starts at the human frame-0 (SNG) via the replay
		// snap, then bunnyhops toward the human path with mode-20 cs->0 accel + a
		// goal-tracking weave. Needs --replay-cmds (the route) like modes 10-12.
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 3);
	}

	else if (mode == 22)
	{
		// Human-faithful actuation: reuses mode 21's nav (where to go) but replaces the
		// forward-only view SNAP with a smooth, rate-limited simulated-view sweep co-rotated
		// with a HELD strafe key -- the technique the human uses to hold max air accel
		// through turns. Needs --replay-cmds (the route) like modes 10-12/21.
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 4);
	}

	else if (mode == 25)
	{
		// Human-mouse speed catch-up: replay the human mouse/timeline, but when live
		// speed is below the human frame's speed by a cvar gap, project a
		// velocity-relative accel wishdir into the recorded view basis. Needs
		// --replay-cmds like modes 10-12/21/22.
		BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, 5);
	}

	else if (mode == 26 || mode == 27 || mode == 28)
	{
		// QWD target-acquisition bunnyhop. Mode 9 proved the ordered segment
		// targets are the right authority, but its alternating strafe is too blunt
		// for transition jumps. Mode 26 keeps QWD acquisition/logging, then uses
		// the mode-22 smooth-mouse idiom against the active segment target.
		// Mode 28 keeps the same target tracker but lets each target carry
		// human view-yaw, signed local commands, and jump/velocity phase.
		float turnrate = cvar("k_fb_moveprobe_s22_turnrate");
		float lead_gain = cvar("k_fb_moveprobe_s22_lead_gain");
		float lead_max = cvar("k_fb_moveprobe_s22_lead_max");
		float flip_deadband = cvar("k_fb_moveprobe_s22_flip_deadband");
		float flip_min_interval = cvar("k_fb_moveprobe_s22_flip_min_int");
		float fwd_frac = cvar("k_fb_moveprobe_s22_fwd");
		float bootstrap_speed = cvar("k_fb_moveprobe_s22_bootstrap_speed");
		float sv_maxspeed = cvar("sv_maxspeed");
		float forwardmove = cvar("k_fb_moveprobe_forwardmove");
		float sidemove = cvar("k_fb_moveprobe_sidemove");
		float goal_yaw, vel_yaw, heading_err, herr, lead, sim_target, dyaw, ft;
		float hor_speed_sq, scheduled_forwardmove, scheduled_sidemove;
		float target_view_yaw = MOVEPROBE_SCHEDULE_NONE;
		float target_velocity_yaw = MOVEPROBE_SCHEDULE_NONE;
		float target_vertical_velocity = MOVEPROBE_SCHEDULE_NONE;
		float target_horizontal_speed = MOVEPROBE_SCHEDULE_NONE;
		float s28_route_weight = cvar("k_fb_moveprobe_s28_route_yaw_weight");
		float s28_cmd_scale = cvar("k_fb_moveprobe_s28_cmd_scale");
		float s28_catchup_move = cvar("k_fb_moveprobe_s28_catchup_move");
		float s28_catchup_gap = cvar("k_fb_moveprobe_s28_catchup_gap");
		float s28_catchup_blend = cvar("k_fb_moveprobe_s28_catchup_blend");
		float s28_catchup_cap = cvar("k_fb_moveprobe_s28_catchup_cap");
		float s28_catchup_numerator = cvar("k_fb_moveprobe_s28_catchup_numerator");
		float s28_jump_vz = cvar("k_fb_moveprobe_s28_jump_vz");
		int s28_disable_qwd_cmds = (int)cvar("k_fb_moveprobe_s28_disable_qwd_cmds");
		int s28_jump_lookahead = (int)cvar("k_fb_moveprobe_s28_jump_lookahead");
		int s28_jump_pulse = (int)cvar("k_fb_moveprobe_s28_jump_pulse");
		int s28_vz_jump_trigger = (int)cvar("k_fb_moveprobe_s28_vz_jump_trigger");
		int s28_catchup_flip = (int)cvar("k_fb_moveprobe_s28_catchup_flip");
		int flip_guard = (int)cvar("k_fb_moveprobe_s22_strafe_flip");
		int jump_guard = 0;
		int qwd_index;
		int want, s;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump, precision, fast_air;
		vec3_t route_direction, cur_dir;

		if (mode == 27)
		{
			vec3_t handoff_points[MOVEPROBE_QWD_MAX_POINTS];
			vec3_t handoff_delta;
			float handoff_radius = cvar("k_fb_moveprobe_s27_handoff_radius");
			float handoff_distance = 999999.0f;
			int handoff_count;
			int handoff_cursor = (int)cvar("k_fb_moveprobe_s27_handoff_cursor");
			int handoff_variant = (int)cvar("k_fb_moveprobe_s27_replay_variant");

			if (handoff_cursor <= 0)
			{
				handoff_cursor = 55;
			}
			if (handoff_variant <= 0)
			{
				handoff_variant = 3;
			}
			if (handoff_radius <= 0.0f)
			{
				handoff_radius = cvar("k_fb_moveprobe_qwd_point_radius");
			}
			if (handoff_radius <= 0.0f)
			{
				handoff_radius = 64.0f;
			}
			handoff_cursor = bound(1, handoff_cursor, MOVEPROBE_REPLAY_MAX_FRAMES - 1);
			handoff_variant = bound(0, handoff_variant, 5);
			handoff_radius = bound(8.0f, handoff_radius, 256.0f);
			if (slot < 0 || slot >= MAX_CLIENTS)
			{
				return;
			}

			if (!moveprobe_s27_handoff_latched[slot])
			{
				handoff_count = BotMoveProbeReadQwdWaypoints(handoff_points);
				if (handoff_count > 0)
				{
					VectorSubtract(handoff_points[0], self->s.v.origin, handoff_delta);
					handoff_distance = VectorLength(handoff_delta);
				}
				if ((moveprobe_replay_cursor[slot] < handoff_cursor)
					|| ((handoff_count > 0) && (handoff_distance > handoff_radius)))
				{
					if (BotApplyMoveProbeReplay(self, slot, jumping, firing, impulse, direction, handoff_variant))
					{
						if (handoff_count > 0)
						{
							VectorSubtract(handoff_points[0], self->s.v.origin, handoff_delta);
							handoff_distance = VectorLength(handoff_delta);
						}
						if ((moveprobe_replay_cursor[slot] < handoff_cursor)
							|| ((handoff_count > 0) && (handoff_distance > handoff_radius)))
						{
							return;
						}
					}
					else
					{
						return;
					}
				}
				moveprobe_s27_handoff_latched[slot] = 1;
			}
		}

		if (!BotMoveProbeQwdActive(self, slot, route_direction))
		{
			return;
		}

		if (turnrate <= 0) turnrate = 450.0f;
		if (lead_gain <= 0) lead_gain = 0.5f;
		if (lead_max <= 0) lead_max = 25.0f;
		if (flip_deadband <= 0) flip_deadband = 20.0f;
		if (flip_min_interval <= 0) flip_min_interval = 0.6f;
		if (fwd_frac < 0) fwd_frac = 0.0f;
		if (bootstrap_speed <= 0) bootstrap_speed = 320.0f;
		if (flip_guard < 0) flip_guard = 0;
		if (sv_maxspeed <= 0) sv_maxspeed = 320.0f;
		if (forwardmove <= 0) forwardmove = sv_maxspeed;
		if (sidemove < 0) sidemove = -sidemove;
		if (sidemove <= 0) sidemove = sv_maxspeed;

		qwd_index = bound(0, moveprobe_qwd_point_index[slot], moveprobe_qwd_point_count[slot] - 1);
		flip_guard = BotMoveProbeReadScheduleInt("k_fb_moveprobe_s26_flip_schedule", qwd_index, flip_guard);
		jump_guard = BotMoveProbeReadScheduleInt("k_fb_moveprobe_s26_jump_schedule", qwd_index, 0);
		turnrate = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_turnrate_schedule", qwd_index, turnrate);
		lead_gain = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_lead_gain_schedule", qwd_index, lead_gain);
		lead_max = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_lead_max_schedule", qwd_index, lead_max);
		scheduled_forwardmove = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_forwardmove_schedule", qwd_index, -1.0f);
		scheduled_sidemove = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_sidemove_schedule", qwd_index, -1.0f);
		if (scheduled_forwardmove >= 0.0f) forwardmove = scheduled_forwardmove;
		if (scheduled_sidemove >= 0.0f) sidemove = scheduled_sidemove;
		if (mode == 28)
		{
			scheduled_forwardmove = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_forwardmove_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			scheduled_sidemove = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s26_sidemove_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			target_view_yaw = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_view_yaw_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			target_velocity_yaw = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_velocity_yaw_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			target_vertical_velocity = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_vertical_velocity_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			target_horizontal_speed = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_horizontal_speed_schedule", qwd_index, MOVEPROBE_SCHEDULE_NONE);
			s28_catchup_move = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_catchup_move_schedule", qwd_index, s28_catchup_move);
			s28_catchup_gap = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_catchup_gap_schedule", qwd_index, s28_catchup_gap);
			s28_catchup_blend = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_catchup_blend_schedule", qwd_index, s28_catchup_blend);
			s28_catchup_cap = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_catchup_cap_schedule", qwd_index, s28_catchup_cap);
			s28_catchup_numerator = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_catchup_numerator_schedule", qwd_index, s28_catchup_numerator);
			s28_catchup_flip = BotMoveProbeReadScheduleInt("k_fb_moveprobe_s28_catchup_flip_schedule", qwd_index, s28_catchup_flip);
		}
		if (turnrate <= 0) turnrate = 450.0f;
		if (lead_gain <= 0) lead_gain = 0.5f;
		if (lead_max <= 0) lead_max = 25.0f;
		if (flip_guard < 0) flip_guard = 0;
		if (jump_guard < 0) jump_guard = 0;
		if (s28_route_weight < 0.0f) s28_route_weight = 0.0f;
		if (s28_cmd_scale <= 0.0f) s28_cmd_scale = 1.0f;
		if (s28_catchup_gap < 0.0f) s28_catchup_gap = 0.0f;
		if (s28_catchup_blend <= 0.0f) s28_catchup_blend = 1.0f;
		s28_catchup_blend = bound(0.0f, s28_catchup_blend, 1.0f);
		if (s28_catchup_cap <= 0.0f) s28_catchup_cap = 1200.0f;
		if (s28_catchup_numerator < 0.0f) s28_catchup_numerator = 0.0f;
		if (s28_jump_vz <= 0.0f) s28_jump_vz = 32.0f;
		if (s28_jump_lookahead < 0) s28_jump_lookahead = 0;
		s28_jump_lookahead = bound(0, s28_jump_lookahead, 4);

		route_direction[2] = 0;
		if (VectorNormalize(route_direction) <= 0)
		{
			return;
		}
		goal_yaw = vectoyaw(route_direction);

		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];
		if (VectorNormalize(cur_dir) <= 0)
		{
			VectorCopy(route_direction, cur_dir);
		}
		vel_yaw = vectoyaw(cur_dir);

		heading_err = goal_yaw - vel_yaw;
		while (heading_err > 180.0f) heading_err -= 360.0f;
		while (heading_err < -180.0f) heading_err += 360.0f;
		herr = (heading_err < 0.0f) ? -heading_err : heading_err;
		precision = onground ? true : false;
		fast_air = (!precision && hor_speed_sq >= bootstrap_speed * bootstrap_speed) ? true : false;

		if (!moveprobe_s22_init[slot])
		{
			if (mode == 28 && target_view_yaw != MOVEPROBE_SCHEDULE_NONE)
			{
				moveprobe_s22_sim_yaw[slot] = target_view_yaw;
			}
			else
			{
				moveprobe_s22_sim_yaw[slot] = (hor_speed_sq > 1.0f) ? vel_yaw : self->fb.desired_angle[YAW];
			}
			moveprobe_s22_strafe_sign[slot] = (heading_err >= 0.0f) ? -1 : 1;
			moveprobe_s22_flip_t[slot] = g_globalvars.time;
			moveprobe_s22_init[slot] = 1;
		}

		want = (heading_err > 0.0f) ? -1 : 1;
		if (flip_guard) want = -want;
		s = moveprobe_s22_strafe_sign[slot];
		if (want != s && herr > flip_deadband
			&& (g_globalvars.time - moveprobe_s22_flip_t[slot]) > flip_min_interval)
		{
			s = want;
			moveprobe_s22_strafe_sign[slot] = s;
			moveprobe_s22_flip_t[slot] = g_globalvars.time;
		}

		if (fast_air)
		{
			lead = lead_gain * heading_err;
			if (lead > lead_max) lead = lead_max;
			if (lead < -lead_max) lead = -lead_max;
			sim_target = vel_yaw + lead;
		}
		else
		{
			sim_target = goal_yaw;
		}
		if (mode == 28)
		{
			if (target_velocity_yaw != MOVEPROBE_SCHEDULE_NONE)
			{
				sim_target = target_velocity_yaw;
			}
			if (target_view_yaw != MOVEPROBE_SCHEDULE_NONE)
			{
				sim_target = target_view_yaw;
			}
			if (s28_route_weight > 0.0f)
			{
				sim_target = BotMoveProbeBlendYaw(sim_target, goal_yaw, s28_route_weight);
			}
		}

		ft = g_globalvars.frametime;
		dyaw = sim_target - moveprobe_s22_sim_yaw[slot];
		while (dyaw > 180.0f) dyaw -= 360.0f;
		while (dyaw < -180.0f) dyaw += 360.0f;
		if (dyaw > turnrate * ft) dyaw = turnrate * ft;
		if (dyaw < -turnrate * ft) dyaw = -turnrate * ft;
		moveprobe_s22_sim_yaw[slot] = anglemod(moveprobe_s22_sim_yaw[slot] + dyaw);

		press_jump = (mode == 28) ? false : (onground && !moveprobe_accel_jump_press[slot]);
		if (jump_guard > 0)
		{
			press_jump = true;
		}
		if (mode == 28)
		{
			int look;
			int state_jump = jump_guard;

			if (s28_vz_jump_trigger > 0
				&& target_vertical_velocity != MOVEPROBE_SCHEDULE_NONE
				&& target_vertical_velocity > s28_jump_vz)
			{
				state_jump = 1;
			}
			for (look = 1; look <= s28_jump_lookahead; look++)
			{
				int future_jump = BotMoveProbeReadScheduleInt("k_fb_moveprobe_s26_jump_schedule",
															  qwd_index + look, 0);
				float future_vz = BotMoveProbeReadScheduleFloat("k_fb_moveprobe_s28_vertical_velocity_schedule",
																qwd_index + look, MOVEPROBE_SCHEDULE_NONE);

				if (future_jump > 0
					|| (s28_vz_jump_trigger > 0
						&& future_vz != MOVEPROBE_SCHEDULE_NONE && future_vz > s28_jump_vz))
				{
					state_jump = 1;
				}
			}
			if (state_jump > 0)
			{
				press_jump = (s28_jump_pulse > 0) ? (onground && !moveprobe_accel_jump_press[slot]) : true;
			}
		}
		moveprobe_accel_jump_press[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = moveprobe_s22_sim_yaw[slot];
		self->fb.desired_angle[ROLL] = 0;
		if (mode == 28 && !s28_disable_qwd_cmds
			&& scheduled_forwardmove != MOVEPROBE_SCHEDULE_NONE
			&& scheduled_sidemove != MOVEPROBE_SCHEDULE_NONE)
		{
			direction[0] = scheduled_forwardmove * s28_cmd_scale;
			direction[1] = scheduled_sidemove * s28_cmd_scale;
		}
		else if (fast_air)
		{
			direction[0] = fwd_frac * forwardmove;
			direction[1] = s * sidemove;
		}
		else if (!precision)
		{
			direction[0] = forwardmove;
			direction[1] = s * sidemove;
		}
		else
		{
			direction[0] = forwardmove;
			direction[1] = 0;
		}
		if (mode == 28 && s28_catchup_move > 0.0f
			&& target_horizontal_speed != MOVEPROBE_SCHEDULE_NONE
			&& target_velocity_yaw != MOVEPROBE_SCHEDULE_NONE)
		{
			float live_speed = sqrt(hor_speed_sq);

			if ((target_horizontal_speed - live_speed) > s28_catchup_gap)
			{
				vec3_t target_angles = { 0, 0, 0 };
				vec3_t target_world;
				vec3_t view_angles;
				float add_forward, add_side;

				target_angles[YAW] = target_velocity_yaw;
				trap_makevectors(target_angles);
				VectorCopy(g_globalvars.v_forward, target_world);
				target_world[2] = 0;
				if (s28_catchup_numerator > 0.0f && live_speed > 1.0f)
				{
					vec3_t speed_dir;
					vec3_t up_vector = { 0, 0, 1 };
					float current_velocity_yaw = vectoyaw(cur_dir);
					float target_delta = BotMoveProbeYawDelta(target_velocity_yaw, current_velocity_yaw);
					float rotation;
					float sign = (target_delta >= 0.0f) ? 1.0f : -1.0f;

					if (s28_catchup_flip > 0)
					{
						sign = -sign;
					}
					VectorCopy(cur_dir, speed_dir);
					if (s28_catchup_numerator < live_speed)
					{
						rotation = acos(s28_catchup_numerator / live_speed) * 180.0f / M_PI;
					}
					else
					{
						rotation = 25.0f;
					}
					RotatePointAroundVector(target_world, up_vector, speed_dir, rotation * sign);
					target_world[2] = 0;
				}
				if (VectorNormalize(target_world) > 0)
				{
					VectorCopy(self->fb.desired_angle, view_angles);
					view_angles[PITCH] = 0;
					view_angles[ROLL] = 0;
					trap_makevectors(view_angles);
					add_forward = DotProduct(g_globalvars.v_forward, target_world) * s28_catchup_move * s28_catchup_blend;
					add_side = DotProduct(g_globalvars.v_right, target_world) * s28_catchup_move * s28_catchup_blend;
					direction[0] += add_forward;
					direction[1] += add_side;
					direction[0] = bound(-s28_catchup_cap, direction[0], s28_catchup_cap);
					direction[1] = bound(-s28_catchup_cap, direction[1], s28_catchup_cap);
				}
			}
		}
		direction[2] = cvar("k_fb_moveprobe_upmove");
		*jumping = press_jump;
	}

	else if (mode == 13)
	{
		// Velocity-aware bunnyhop accelerator (no replay). On the GROUND: run
		// straight along current velocity to build speed and press jump (toggled,
		// so it releases in the air -- holding +jump only jumps once). AIRBORNE:
		// aim the wish-direction at the speed-optimal angle off current velocity
		// (the acos rule ApplyPhysics uses); the engine's capped air-accel turns
		// velocity only slightly per frame, so a large wish-angle is correct and
		// grows the speed -> a bunnyhop that accelerates toward the QW ceiling.
		// Rotating on the ground would just spin the bot (ground accel redirects
		// fully), so the strafe is gated to the air. numerator is a tunable cvar;
		// metric is horizontal speed. See docs/08_DECISION_LOG.md.
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float fixed_angle = cvar("k_fb_moveprobe_accel_angle");      // >0: override strafe angle (deg)
		float alternate = cvar("k_fb_moveprobe_accel_alternate");    // >0: flip strafe side each hop (S-strafe)
		float sv_maxspeed = cvar("sv_maxspeed");
		float max_incr;
		float hor_speed_sq;
		float rotation;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t cur_dir;
		vec3_t proposed_dir;
		vec3_t up_vector = { 0, 0, 1 };

		if (numerator <= 0)
		{
			// numerator = target currentspeed K (v.wishdir, qu/s). QW air accel
			// adds speed only while currentspeed < the ~30 air cap, and the speed
			// gain scales with currentspeed, so the optimal is to hold currentspeed
			// just under the cap: K ~= 30 - sv_accelerate*30*frametime ~= 26.
			numerator = 26.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (moveprobe_accel_strafe_sign[slot] == 0)
		{
			moveprobe_accel_strafe_sign[slot] = 1;
		}
		max_incr = numerator;

		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];

		if (VectorNormalize(cur_dir) <= 0)
		{
			// At rest: seed the heading from the current view.
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1;
				cur_dir[1] = 0;
				cur_dir[2] = 0;
			}
		}

		if (onground)
		{
			// Build/keep speed running straight; no strafe rotation on the ground.
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			if (fixed_angle > 0)
			{
				// Fixed-angle override: hold a constant wish-angle off velocity
				// regardless of speed. The acos rule below widens toward 90 as
				// speed grows (a tight circle that bleeds speed); a smaller fixed
				// angle keeps each frame near the speed-optimal turn. Sweepable
				// purely by cvar -- no rebuild needed to retune.
				rotation = fixed_angle;
			}
			else if (hor_speed_sq > max_incr * max_incr)
			{
				// Speed-optimal wish-angle: hold v.wishdir at the target K just
				// under the air cap. angle = acos(K / |hor velocity|). Rises toward
				// 90 as speed grows, which is correct -- the higher the speed, the
				// less the velocity may turn while still leaving headroom under the
				// cap. (Earlier acos((K/speed)^2) sat at ~90 with v.wishdir~0, which
				// adds speed perpendicular-only and barely grows |v|.)
				rotation = acos(max_incr / sqrt(hor_speed_sq)) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir,
									rotation * moveprobe_accel_strafe_sign[slot]);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		// Toggle jump: press on the ground only if it was released last frame, so
		// the button releases in the air and re-fires on each landing.
		press_jump = onground && !moveprobe_accel_jump_press[slot];
		moveprobe_accel_jump_press[slot] = press_jump;

		// Alternating S-strafe: flip the strafe side on each takeoff so the path
		// stays roughly straight (left-then-right) instead of a one-way circle.
		// Off (default) keeps a constant-direction circle-strafe.
		if (alternate > 0 && press_jump)
		{
			moveprobe_accel_strafe_sign[slot] = -moveprobe_accel_strafe_sign[slot];
		}

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}
	else if (mode == 14)
	{
		// Orbit + carve (v4). The DIRECTION is a smooth tangent to a circle of radius
		// R about a center C (the human trick5 centroid), gently biased to hold R --
		// a big wall-hugging loop (R=850), not a tight infield circle. The CARVE is
		// the mode-13 air-strafe around the actual VELOCITY. v4's fix (from the user's
		// tricks-direction.jpg + the demo showing the bot "watching the center"):
		// DECOUPLE look from push. v3 pointed the VIEW at the carve wishdir (~85 deg
		// off velocity) -> on a tight orbit the bot stared at the center. v4 points
		// the view along the loop tangent (look AHEAD around the round wall, the
		// travel direction) and emits the carve as forward/side strafe -- the dance:
		// face your partner, footwork underneath. See experiments/bunnyhop_mastery.
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float orbit_dir = cvar("k_fb_moveprobe_orbit_dir");          // <0 = CW, else CCW
		float cx = cvar("k_fb_moveprobe_orbit_cx");                  // orbit center X
		float cy = cvar("k_fb_moveprobe_orbit_cy");                  // orbit center Y
		float radius = cvar("k_fb_moveprobe_orbit_radius");          // target radius (qu)
		float radius_gain = cvar("k_fb_moveprobe_orbit_radius_gain");// inward/outward pull
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float sv_maxspeed = cvar("sv_maxspeed");
		float carve_deadband = 20.0f;  // deg: |heading error| below which we S-strafe
		int orbit_sign;
		int carve_sign;
		float spd;
		float rotation;
		float base_yaw;
		float vel_yaw;
		float error;
		float r;
		vec3_t base_dir;
		vec3_t cur_dir;
		vec3_t radial;
		vec3_t proposed_dir;
		vec3_t up_vector = { 0, 0, 1 };
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;

		if (numerator <= 0)
		{
			numerator = 26.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (radius <= 0)
		{
			// v4: hug the round wall (the user's drawing = a big loop near the
			// boundary, center empty), not a tight circle about the centroid. The
			// human trick5 loop spans bbox ~1750 -> half-extent ~875. v3's 624
			// orbited too tight, which (with the old view = wishdir) read as
			// "circling/watching the center". 850 fills the room to the wall.
			radius = 850.0f;
		}
		if (radius_gain <= 0)
		{
			radius_gain = 0.6f;
		}
		if (cx == 0.0f && cy == 0.0f)
		{
			cx = 2523.0f;  // human trick5 centroid
			cy = -2554.0f;
		}
		orbit_sign = (orbit_dir < 0) ? -1 : 1;

		spd = sqrt(self->s.v.velocity[0] * self->s.v.velocity[0]
				   + self->s.v.velocity[1] * self->s.v.velocity[1]);

		// Velocity heading (the carve reference); seed from view at rest.
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1;
				cur_dir[1] = 0;
				cur_dir[2] = 0;
			}
		}

		// DIRECTION layer -- smooth center orbit. base heading = tangent to the circle
		// of radius `radius` about (cx,cy) at the bot's position, gently biased toward
		// the target radius. The tangent turns slowly as the bot orbits (no ray jerk).
		radial[0] = self->s.v.origin[0] - cx;
		radial[1] = self->s.v.origin[1] - cy;
		radial[2] = 0;
		r = sqrt(radial[0] * radial[0] + radial[1] * radial[1]);
		if (r < 1.0f)
		{
			VectorCopy(cur_dir, base_dir);  // at the center: no tangent, use velocity
		}
		else
		{
			float rux = radial[0] / r;          // outward radial unit
			float ruy = radial[1] / r;
			float tx = -ruy * orbit_sign;       // tangent (CCW for orbit_sign +1)
			float ty = rux * orbit_sign;
			float radius_error = (r - radius) / radius;  // >0 too far out

			if (radius_error > 1.0f)
			{
				radius_error = 1.0f;
			}
			if (radius_error < -1.0f)
			{
				radius_error = -1.0f;
			}
			// too far -> bias inward (-radial unit); too close -> bias outward (+).
			base_dir[0] = tx - rux * radius_gain * radius_error;
			base_dir[1] = ty - ruy * radius_gain * radius_error;
			base_dir[2] = 0;
			if (VectorNormalize(base_dir) <= 0)
			{
				VectorCopy(cur_dir, base_dir);
			}
		}
		base_yaw = vectoyaw(base_dir);
		moveprobe_orbit_yaw[slot] = base_yaw;
		if (!moveprobe_orbit_init[slot])
		{
			moveprobe_orbit_init[slot] = 1;
			moveprobe_orbit_sign[slot] = 1;
		}

		// CARVE layer -- around VELOCITY (full mode-13 accel). Toggle jump and flip
		// the carve side per hop (S-strafe); when the heading error to the base
		// exceeds the deadband, lock the side toward the base to steer onto the orbit.
		press_jump = onground && !moveprobe_orbit_jump[slot];
		moveprobe_orbit_jump[slot] = press_jump;
		if (press_jump)
		{
			moveprobe_orbit_sign[slot] = -moveprobe_orbit_sign[slot];
		}
		vel_yaw = vectoyaw(cur_dir);
		error = base_yaw - vel_yaw;
		while (error > 180)
		{
			error -= 360;
		}
		while (error < -180)
		{
			error += 360;
		}
		if (error > carve_deadband)
		{
			moveprobe_orbit_sign[slot] = 1;   // base is left of velocity -> carve left
		}
		else if (error < -carve_deadband)
		{
			moveprobe_orbit_sign[slot] = -1;  // base is right -> carve right
		}
		carve_sign = moveprobe_orbit_sign[slot];

		if (onground)
		{
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			if (spd > numerator)
			{
				rotation = acos(numerator / spd) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir, rotation * carve_sign);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		// EMIT -- decouple LOOK from PUSH (the dance: face where you travel, carve
		// underneath). v3 set the view = wishdir (~85 deg off velocity) and pushed
		// pure forwardmove, so on a tight orbit the bot LITERALLY LOOKED AT THE
		// CENTER (the "watching the center" the demo showed). v4: the view is
		// base_yaw (the loop tangent -> look AHEAD along the round wall, in the
		// travel direction), and the carve wishdir (proposed_dir, unchanged from v3)
		// is emitted as forward/side by projecting it onto the view's own basis (the
		// established idiom, ~lines 2109). wishvel == proposed_dir * maxspeed exactly
		// (identical accel to v3), but the bot now faces the direction it travels --
		// mostly strafe + look-ahead, like a human, not a forward-stare at the infield.
		{
			vec3_t view_ang = { 0, base_yaw, 0 };

			trap_makevectors(view_ang);
			self->fb.desired_angle[PITCH] = 0;
			self->fb.desired_angle[YAW] = base_yaw;
			self->fb.desired_angle[ROLL] = 0;
			direction[0] = DotProduct(g_globalvars.v_forward, proposed_dir) * sv_maxspeed;
			direction[1] = DotProduct(g_globalvars.v_right, proposed_dir) * sv_maxspeed;
			direction[2] = 0;
		}
		*jumping = press_jump;
	}
	else if (mode == 15)
	{
		// Coupled circle-strafe (the human technique, from trick5.cmds Step-0 analysis).
		// The human at speed uses forwardmove=0, sidemove=+/-400 held for SECONDS, and a
		// view that rotates CONTINUOUSLY in the direction COUPLED to the strafe sign
		// (side+ -> turn CW / yaw decreasing; side- -> turn CCW) at ~150 deg/s. Velocity
		// chases the steadily-rotating view in the air-accel pocket, so speed BUILDS (929
		// -> 1072 over 7 s in the demo) -- there is NO 540 ceiling; that was mode-14's
		// slow heading + per-hop S-strafe killing the accel. Confinement: orbit a FIXED
		// center, flipping both strafe sign + coupled turn on a timer / radius drift (NOT
		// reactive wall-traceline, which chattered and collapsed the pocket -> crash to ~50).
		// See experiments/bunnyhop_mastery/evidence/STEP0-circle-strafe-overturns-ceiling.md.
		float side_move = cvar("k_fb_moveprobe_cstrafe_side");        // sidemove magnitude
		float turn_rate = cvar("k_fb_moveprobe_cstrafe_turnrate");   // deg/s, continuous view turn
		float flip_period = cvar("k_fb_moveprobe_cstrafe_flip_period"); // s between forced flips
		float max_radius = cvar("k_fb_moveprobe_cstrafe_max_radius"); // qu: flip if drifting past this
		float cx = cvar("k_fb_moveprobe_cstrafe_cx");                // orbit center X
		float cy = cvar("k_fb_moveprobe_cstrafe_cy");                // orbit center Y
		float sv_maxspeed = cvar("sv_maxspeed");
		float view_yaw;
		float r;
		float since;
		int side;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t flat_vel;
		vec3_t view_ang;

		if (side_move <= 0)
		{
			side_move = 400.0f;
		}
		if (turn_rate <= 0)
		{
			turn_rate = 150.0f;  // human median ~154 deg/s (2 deg/frame @ 77 fps)
		}
		if (flip_period <= 0)
		{
			flip_period = 2.5f;  // shorter arc -> tighter confinement (figure-8 lobe)
		}
		if (max_radius <= 0)
		{
			max_radius = 520.0f; // flip if the orbit drifts past this from center
		}
		if (cx == 0.0f && cy == 0.0f)
		{
			cx = 2523.0f;  // human trick5 centroid (~ room center)
			cy = -2554.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}

		// Seed the integrated view from the current velocity heading (or rest view).
		if (!moveprobe_cstrafe_init[slot])
		{
			flat_vel[0] = self->s.v.velocity[0];
			flat_vel[1] = self->s.v.velocity[1];
			flat_vel[2] = 0;
			if (VectorNormalize(flat_vel) > 0)
			{
				moveprobe_cstrafe_view[slot] = vectoyaw(flat_vel);
			}
			else
			{
				moveprobe_cstrafe_view[slot] = self->fb.desired_angle[YAW];
			}
			moveprobe_cstrafe_side[slot] = 1;
			moveprobe_cstrafe_flip_t[slot] = g_globalvars.time;
			moveprobe_cstrafe_init[slot] = 1;
		}
		side = moveprobe_cstrafe_side[slot];

		// Confinement: orbit a FIXED center. Flip both strafe sign and the coupled turn
		// direction together on the timer OR when the orbit drifts past max_radius from the
		// center. A >=1 s minimum between flips prevents the reactive wall-chatter that
		// decelerated the first cut (rapid reversals collapse the air-strafe pocket). The
		// figure-8 emerges from the periodic flips while the bot stays near the center.
		r = sqrt((self->s.v.origin[0] - cx) * (self->s.v.origin[0] - cx)
				 + (self->s.v.origin[1] - cy) * (self->s.v.origin[1] - cy));
		since = g_globalvars.time - moveprobe_cstrafe_flip_t[slot];
		if (since >= 1.0f && (since >= flip_period || r > max_radius))
		{
			side = -side;
			moveprobe_cstrafe_side[slot] = side;
			moveprobe_cstrafe_flip_t[slot] = g_globalvars.time;
		}

		// Integrate the view continuously in the coupled direction. side+ (strafe right)
		// turns the view CW (yaw decreasing); side- turns CCW (yaw increasing).
		moveprobe_cstrafe_view[slot] -= side * turn_rate * g_globalvars.frametime;
		view_yaw = moveprobe_cstrafe_view[slot];

		// Jump every ground frame (continuous bunnyhop).
		press_jump = onground && !moveprobe_cstrafe_jump[slot];
		moveprobe_cstrafe_jump[slot] = press_jump;

		// Emit: look along the integrated view; pure sidemove in the strafe direction.
		view_ang[0] = 0;
		view_ang[1] = view_yaw;
		view_ang[2] = 0;
		trap_makevectors(view_ang);
		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = view_yaw;
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = 0;                      // forwardmove (human used 0 at speed)
		direction[1] = side * side_move;       // sidemove (+ = right)
		direction[2] = 0;
		*jumping = press_jump;
	}
	else if (mode == 16)
	{
		// Curling air-strafe with center-hold confinement (the synthesis of the
		// CONFIRMED physics + the user's model). ACCEL = mode 13's proven one-way
		// carve: wishdir at acos(K/|v|) off the current velocity (K~26, near the real
		// optimum acos(15/|v|)), held ONE direction -> velocity curls into a circle of
		// radius ~|v|^2/2310 while air-accel (the real 30-cap) builds speed. That alone
		// reaches 656 but SPIRALS into a wall (mode 13's failure). CONFINEMENT (the
		// missing layer): when the bot drifts past max_radius from the room center OR a
		// forward traceline sees a wall, FLIP the curl direction to whichever side
		// steers velocity back toward the center -- with a long cooldown so it does
		// LONG arcs (not the per-hop pump / wall-chatter that killed mode 15). The
		// alternating long arcs are the figure-8; the carve never stops, so speed keeps
		// climbing past 656 toward the human band. See evidence/PHYSICS-confirmed-air-accel.md.
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float radius_div = cvar("k_fb_moveprobe_curl_radius_div");   // R_natural = v^2/div (~2310)
		float radius_min = cvar("k_fb_moveprobe_curl_radius_min");   // qu clamp on target radius
		float radius_max = cvar("k_fb_moveprobe_curl_radius_max");   // qu clamp (keep inside room)
		float radius_gain = cvar("k_fb_moveprobe_curl_radius_gain"); // deg of carve per qu of radius error
		float angle_span = cvar("k_fb_moveprobe_curl_angle_span");   // max deg the centering may shift the carve
		float radius_damp = cvar("k_fb_moveprobe_curl_radius_damp"); // deg per (qu/s) of radial velocity (D term)
		float engage_speed = cvar("k_fb_moveprobe_curl_engage_speed"); // pure accel below this, center above
		float cx = cvar("k_fb_moveprobe_curl_cx");
		float cy = cvar("k_fb_moveprobe_curl_cy");
		float sv_maxspeed = cvar("sv_maxspeed");
		float spd;
		float rotation;
		float r;
		int curl_sign;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t cur_dir;
		vec3_t proposed_dir;
		vec3_t up_vector = { 0, 0, 1 };

		if (numerator <= 0)
		{
			numerator = 26.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (radius_div <= 0)
		{
			radius_div = 2310.0f; // v^2/2310: the 30-cap natural curl radius (R=335 at 880)
		}
		if (radius_min <= 0)
		{
			radius_min = 120.0f;
		}
		if (radius_max <= 0)
		{
			radius_max = 560.0f; // keep the orbit inside the ~1008 room half-width
		}
		if (radius_gain <= 0)
		{
			radius_gain = 0.045f;
		}
		if (angle_span <= 0)
		{
			angle_span = 25.0f;
		}
		if (radius_damp <= 0)
		{
			radius_damp = 0.03f; // D term: damps the radius oscillation (PD, not just P)
		}
		if (engage_speed <= 0)
		{
			engage_speed = 420.0f; // bootstrap with PURE accel below this; center above
		}
		if (cx == 0.0f && cy == 0.0f)
		{
			cx = 2523.0f;  // human trick5 centroid (~ room center)
			cy = -2554.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (!moveprobe_curl_init[slot])
		{
			moveprobe_curl_sign[slot] = 1;
			moveprobe_curl_dirset[slot] = 0;
			moveprobe_curl_init[slot] = 1;
		}

		spd = sqrt(self->s.v.velocity[0] * self->s.v.velocity[0]
				   + self->s.v.velocity[1] * self->s.v.velocity[1]);
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1; cur_dir[1] = 0; cur_dir[2] = 0;
			}
		}

		// Curl direction: choose ONCE from geometry so the curl orbits the center
		// (center on the curl side of velocity); fixed thereafter -> no flips.
		r = sqrt((self->s.v.origin[0] - cx) * (self->s.v.origin[0] - cx)
				 + (self->s.v.origin[1] - cy) * (self->s.v.origin[1] - cy));
		if (!moveprobe_curl_dirset[slot] && spd > 100.0f)
		{
			float tx = cx - self->s.v.origin[0];
			float ty = cy - self->s.v.origin[1];
			float cross = self->s.v.velocity[0] * ty - self->s.v.velocity[1] * tx;

			moveprobe_curl_sign[slot] = (cross >= 0) ? 1 : -1;
			moveprobe_curl_dirset[slot] = 1;
		}
		curl_sign = moveprobe_curl_sign[slot];

		// ACCEL + CENTERED CONFINEMENT. The carve is mode-13's accel-optimal angle off
		// velocity (acos(K/v)); the centering GENTLY shifts that angle by the radius
		// error so the bot holds its distance-from-center near the natural curl radius
		// R = v^2/div -> a centered orbit that grows with speed and stays in the room,
		// with no speed-killing flips. Too far out -> tighter carve (pull in); too close
		// -> wider carve (drift out).
		if (onground)
		{
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			float r_target = spd * spd / radius_div;
			float angle;

			if (r_target < radius_min)
			{
				r_target = radius_min;
			}
			if (r_target > radius_max)
			{
				r_target = radius_max;
			}
			if (spd > numerator)
			{
				rotation = acos(numerator / spd) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			// Bootstrap: below engage_speed run PURE accel (one-sided carve) so the bot
			// can always build up; only engage the centering once it is moving fast.
			if (spd < engage_speed)
			{
				angle = rotation;
			}
			else
			{
				// PD centering: P on radius error + D on radial velocity (the rate the
				// bot is drifting in/out) -> damps the proportional-only oscillation that
				// produced the ~6 s crash-cycle.
				float radial_vel = (r > 1.0f)
					? (self->s.v.velocity[0] * (self->s.v.origin[0] - cx)
					   + self->s.v.velocity[1] * (self->s.v.origin[1] - cy)) / r
					: 0.0f;  // qu/s, positive = moving outward

				angle = rotation + radius_gain * (r - r_target) + radius_damp * radial_vel;
				if (angle < rotation - angle_span)
				{
					angle = rotation - angle_span;
				}
				if (angle > rotation + angle_span)
				{
					angle = rotation + angle_span;
				}
			}
			if (angle > 89.0f)
			{
				angle = 89.0f;
			}
			if (angle < 5.0f)
			{
				angle = 5.0f;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir, angle * curl_sign);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		press_jump = onground && !moveprobe_curl_jump[slot];
		moveprobe_curl_jump[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}
	else if (mode == 17)
	{
		// Smooth figure-8 lobe-switch. One-way carve (mode-13 accel) builds speed; when
		// a wall is ahead or the bot drifts past max_radius, the curl direction reverses
		// SMOOTHLY -- the effective curl ramps target -> -target over ramp_time, passing
		// through 0 (a brief straight) -- so the lobe-switch costs ~the human's ~20%
		// rather than the violent hard-flip crash. The two smooth lobes are the figure-8.
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float ramp_time = cvar("k_fb_moveprobe_fig8_ramp_time");      // s for a full curl reversal
		float max_radius = cvar("k_fb_moveprobe_fig8_max_radius");    // qu: switch lobe past this
		float wall_thresh = cvar("k_fb_moveprobe_fig8_wall_thresh");  // qu base trace reach
		float flip_cooldown = cvar("k_fb_moveprobe_fig8_flip_cooldown"); // s min between switches
		float cx = cvar("k_fb_moveprobe_fig8_cx");
		float cy = cvar("k_fb_moveprobe_fig8_cy");
		float sv_maxspeed = cvar("sv_maxspeed");
		float spd;
		float rotation;
		float r;
		float since;
		float ramp;
		float step;
		int target;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		qbool switch_lobe = false;
		vec3_t cur_dir;
		vec3_t proposed_dir;
		vec3_t flat_vel;
		vec3_t up_vector = { 0, 0, 1 };

		if (numerator <= 0)
		{
			numerator = 18.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (ramp_time <= 0)
		{
			ramp_time = 0.5f;
		}
		if (max_radius <= 0)
		{
			max_radius = 620.0f;
		}
		if (wall_thresh <= 0)
		{
			wall_thresh = 300.0f;
		}
		if (flip_cooldown <= 0)
		{
			flip_cooldown = 1.5f;
		}
		if (cx == 0.0f && cy == 0.0f)
		{
			cx = 2523.0f;
			cy = -2554.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (!moveprobe_fig8_init[slot])
		{
			moveprobe_fig8_target[slot] = 1;
			moveprobe_fig8_ramp[slot] = 1.0f;
			moveprobe_fig8_flip_t[slot] = g_globalvars.time;
			moveprobe_fig8_init[slot] = 1;
		}

		spd = sqrt(self->s.v.velocity[0] * self->s.v.velocity[0]
				   + self->s.v.velocity[1] * self->s.v.velocity[1]);
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t va = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(va);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1; cur_dir[1] = 0; cur_dir[2] = 0;
			}
		}

		r = sqrt((self->s.v.origin[0] - cx) * (self->s.v.origin[0] - cx)
				 + (self->s.v.origin[1] - cy) * (self->s.v.origin[1] - cy));
		since = g_globalvars.time - moveprobe_fig8_flip_t[slot];

		// Lobe-switch trigger: wall ahead OR drifted past max_radius, cooldown-gated.
		if (since >= flip_cooldown)
		{
			if (r > max_radius)
			{
				switch_lobe = true;
			}
			else
			{
				flat_vel[0] = self->s.v.velocity[0];
				flat_vel[1] = self->s.v.velocity[1];
				flat_vel[2] = 0;
				if (VectorNormalize(flat_vel) > 0)
				{
					vec3_t te;
					float reach = wall_thresh + 0.3f * spd;

					VectorMA(self->s.v.origin, reach, flat_vel, te);
					traceline(PASSVEC3(self->s.v.origin), PASSVEC3(te), false, self);
					if (g_globalvars.trace_fraction < 1.0f)
					{
						switch_lobe = true;
					}
				}
			}
		}
		if (switch_lobe)
		{
			float tx = cx - self->s.v.origin[0];
			float ty = cy - self->s.v.origin[1];
			float cross = self->s.v.velocity[0] * ty - self->s.v.velocity[1] * tx;

			moveprobe_fig8_target[slot] = (cross >= 0) ? 1 : -1;
			moveprobe_fig8_flip_t[slot] = g_globalvars.time;
		}
		target = moveprobe_fig8_target[slot];

		// Ramp the effective curl toward the target (smooth reversal through 0).
		ramp = moveprobe_fig8_ramp[slot];
		step = 2.0f * g_globalvars.frametime / ramp_time;
		if (ramp < target)
		{
			ramp += step;
			if (ramp > target)
			{
				ramp = target;
			}
		}
		else if (ramp > target)
		{
			ramp -= step;
			if (ramp < target)
			{
				ramp = target;
			}
		}
		moveprobe_fig8_ramp[slot] = ramp;

		if (onground)
		{
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			if (spd > numerator)
			{
				rotation = acos(numerator / spd) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir, rotation * ramp);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		press_jump = onground && !moveprobe_fig8_jump[slot];
		moveprobe_fig8_jump[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}
	else if (mode == 18)
	{
		// Continuous-rotation ORBIT bunnyhop. Validated offline in qwsim (faithful to the
		// human trick5 air-accel to 0.1%): rotate the view -- and thus the perpendicular
		// side-strafe wish-direction -- open-loop at a fixed OMEGA deg/s and let velocity
		// CHASE it. QW air-accel then builds speed to the OMEGA-set steady state while
		// R = v/omega keeps the orbit inside the room: median ~895 CONFINED (maxdist ~1005 <
		// trick.bsp half-width ~1008) at OMEGA ~205. The "540 single-orbit ceiling" was a
		// controller artifact; the old curl carve sat at ~130 because it computed a
		// velocity-RELATIVE carve (sign-fragile). Open-loop rotation is self-stabilizing.
		// Ground: build speed straight + jump. Air: orbit. A circle (no reversal) -- the
		// figure-8 is a later mode. cvars: orbit_omega, orbit_flip.
		float omega = cvar("k_fb_moveprobe_orbit_omega");       // wish-dir rotation rate, deg/s
		float flip = cvar("k_fb_moveprobe_orbit_flip");         // 1: mirror strafe sign (live convention check)
		float sv_maxspeed = cvar("sv_maxspeed");
		float strafe_sign;
		float heading;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t cur_dir;

		if (omega <= 0)
		{
			omega = 205.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}

		// Current horizontal heading (fall back to the view at a standstill).
		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1;
				cur_dir[1] = 0;
				cur_dir[2] = 0;
			}
		}
		heading = vectoyaw(cur_dir);

		// Bunnyhop: toggle jump so it releases in the air and re-fires on landing (KTX only
		// jumps on a press edge -- a held +jump jumps once).
		press_jump = onground && !moveprobe_orbit18_jump[slot];
		moveprobe_orbit18_jump[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[ROLL] = 0;
		if (onground)
		{
			// GROUND (1 frame/hop): run straight along velocity -- a side-strafe here would
			// spin the bot, since ground accel fully redirects velocity to the wish-dir.
			// Resync the orbit view to the real heading so each hop's air-orbit rotates from
			// where the bot actually is (keeps the open-loop rotation from drifting away).
			moveprobe_orbit18_yaw[slot] = heading;
			self->fb.desired_angle[YAW] = heading;
			direction[0] = sv_maxspeed;
			direction[1] = 0;
			direction[2] = 0;
		}
		else
		{
			// AIR: rotate the orbit view at OMEGA from the landing heading and pure-side-strafe
			// perpendicular to it -> air-accel builds speed AND curls the path (so it never
			// runs straight into a wall). Default strafe sign accelerates for a +omega (CCW)
			// rotation (matches the validated sim); set k_fb_moveprobe_orbit_flip 1 if the
			// live build mirrors sidemove -- speed must BUILD, not collapse.
			moveprobe_orbit18_yaw[slot] = anglemod(moveprobe_orbit18_yaw[slot]
												   + omega * g_globalvars.frametime);
			strafe_sign = (flip > 0) ? 1.0f : -1.0f;
			self->fb.desired_angle[YAW] = moveprobe_orbit18_yaw[slot];
			direction[0] = 0;
			direction[1] = strafe_sign * sv_maxspeed;
			direction[2] = 0;
		}
		*jumping = press_jump;
	}
	else if (mode == 19)
	{
		// Wall-aware retention bunnyhop = mode 13 + live wall sensing. Same
		// velocity-relative air-accel as mode 13 (the acos rule, toggle jump,
		// optional S-strafe), but each AIR frame casts a forward traceline along
		// velocity and FLIPS the strafe side (debounced) when a wall is within
		// s19_wall qu -- so the accelerating curl turns away from geometry instead
		// of crashing into it. That confinement-into-walls is the retention cap
		// that holds mode 13 at ~600 on trick.bsp (accel is already near-optimal;
		// the corpus of 848 high-speed POV demos confirms the cruise wish-angle).
		// The wall flip is the data-diagnosed retention fix via live trap_traceline
		// (no offline BSP). cvars: reuses k_fb_moveprobe_accel_* plus
		// k_fb_moveprobe_s19_wall (forward look qu) and k_fb_moveprobe_s19_wall_cd
		// (frames between wall flips).
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float fixed_angle = cvar("k_fb_moveprobe_accel_angle");      // >0: override strafe angle (deg)
		float alternate = cvar("k_fb_moveprobe_accel_alternate");    // >0: flip strafe side each hop
		float wall_thresh = cvar("k_fb_moveprobe_s19_wall");         // forward look distance (qu)
		float wall_cd = cvar("k_fb_moveprobe_s19_wall_cd");          // frames between wall flips (debounce)
		float flip_frames = cvar("k_fb_moveprobe_s19_flip_frames"); // >0: S-strafe flip every N air frames (corpus ~22)
		float sv_maxspeed = cvar("sv_maxspeed");
		float max_incr;
		float hor_speed_sq;
		float rotation;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t cur_dir;
		vec3_t proposed_dir;
		vec3_t up_vector = { 0, 0, 1 };

		if (numerator <= 0)
		{
			numerator = 26.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (wall_thresh <= 0)
		{
			wall_thresh = 220.0f;
		}
		if (wall_cd <= 0)
		{
			wall_cd = 8.0f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (moveprobe_accel_strafe_sign[slot] == 0)
		{
			moveprobe_accel_strafe_sign[slot] = 1;
		}
		max_incr = numerator;

		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];

		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1;
				cur_dir[1] = 0;
				cur_dir[2] = 0;
			}
		}

		if (moveprobe_s19_wallcd[slot] > 0)
		{
			moveprobe_s19_wallcd[slot] -= 1;
		}

		if (onground)
		{
			moveprobe_s19_flipframes[slot] = 0;
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			// Retention: look ahead along velocity; flip the strafe side (debounced)
			// when a wall is close so the curl turns away from geometry.
			vec3_t ahead;

			VectorMA(self->s.v.origin, wall_thresh, cur_dir, ahead);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(ahead), false, self);
			if (g_globalvars.trace_fraction < 1.0f && moveprobe_s19_wallcd[slot] <= 0)
			{
				moveprobe_accel_strafe_sign[slot] = -moveprobe_accel_strafe_sign[slot];
				moveprobe_s19_wallcd[slot] = (int) wall_cd;
				moveprobe_s19_flipframes[slot] = 0;
			}

			// Serpentine cadence: flip the strafe side every flip_frames air frames so
			// the opposing curls cancel (net ~straight S-strafe) and speed builds over a
			// long net-straight path instead of a one-way circle that fills the room.
			// Corpus median ~22 frames between flips.
			if (flip_frames > 0)
			{
				moveprobe_s19_flipframes[slot] += 1;
				if (moveprobe_s19_flipframes[slot] >= (int) flip_frames)
				{
					moveprobe_accel_strafe_sign[slot] = -moveprobe_accel_strafe_sign[slot];
					moveprobe_s19_flipframes[slot] = 0;
				}
			}

			if (fixed_angle > 0)
			{
				rotation = fixed_angle;
			}
			else if (hor_speed_sq > max_incr * max_incr)
			{
				rotation = acos(max_incr / sqrt(hor_speed_sq)) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir,
									rotation * moveprobe_accel_strafe_sign[slot]);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		press_jump = onground && !moveprobe_accel_jump_press[slot];
		moveprobe_accel_jump_press[slot] = press_jump;

		if (alternate > 0 && press_jump)
		{
			moveprobe_accel_strafe_sign[slot] = -moveprobe_accel_strafe_sign[slot];
		}

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}
	else if (mode == 20)
	{
		// Continuous open-space steering bunnyhop = mode-19 accel core, but instead of a
		// reactive wall-flip it traces forward-left (+45 deg) and forward-right (-45 deg)
		// off velocity each AIR frame and curls toward the MORE OPEN side. Near the right
		// wall it curls left (back toward center); near the left wall it curls right -> a
		// wall-to-wall net-straight weave that never reaches a wall to crash on, while
		// keeping the low-numerator (cs->0) accel. cvars: reuses k_fb_moveprobe_accel_* +
		// k_fb_moveprobe_s19_wall (steer look distance, default 400) +
		// k_fb_moveprobe_s20_deadband (open-fraction margin to switch curl side; anti-chatter).
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float fixed_angle = cvar("k_fb_moveprobe_accel_angle");
		float look = cvar("k_fb_moveprobe_s19_wall");
		float deadband = cvar("k_fb_moveprobe_s20_deadband");
		float sv_maxspeed = cvar("sv_maxspeed");
		float max_incr;
		float hor_speed_sq;
		float rotation;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump;
		vec3_t cur_dir;
		vec3_t proposed_dir;
		vec3_t up_vector = { 0, 0, 1 };

		if (numerator <= 0)
		{
			numerator = 11.0f;
		}
		if (bootstrap_deg <= 0)
		{
			bootstrap_deg = 25.0f;
		}
		if (look <= 0)
		{
			look = 400.0f;
		}
		if (deadband <= 0)
		{
			deadband = 0.08f;
		}
		if (sv_maxspeed <= 0)
		{
			sv_maxspeed = 320.0f;
		}
		if (moveprobe_accel_strafe_sign[slot] == 0)
		{
			moveprobe_accel_strafe_sign[slot] = 1;
		}
		max_incr = numerator;

		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];

		if (VectorNormalize(cur_dir) <= 0)
		{
			vec3_t view_angles = { 0, self->fb.desired_angle[YAW], 0 };

			trap_makevectors(view_angles);
			VectorCopy(g_globalvars.v_forward, cur_dir);
			cur_dir[2] = 0;
			if (VectorNormalize(cur_dir) <= 0)
			{
				cur_dir[0] = 1;
				cur_dir[1] = 0;
				cur_dir[2] = 0;
			}
		}

		if (onground)
		{
			VectorCopy(cur_dir, proposed_dir);
		}
		else
		{
			// Steer toward open space: trace +/-45 deg off velocity; curl toward the side
			// with the larger trace fraction (more open). Deadband holds the current curl
			// when both sides are similarly open (keeps the weave going).
			vec3_t left_dir, right_dir, lp, rp;
			float left_open, right_open;

			RotatePointAroundVector(left_dir, up_vector, cur_dir, 45.0f);
			RotatePointAroundVector(right_dir, up_vector, cur_dir, -45.0f);
			VectorMA(self->s.v.origin, look, left_dir, lp);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(lp), false, self);
			left_open = g_globalvars.trace_fraction;
			VectorMA(self->s.v.origin, look, right_dir, rp);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(rp), false, self);
			right_open = g_globalvars.trace_fraction;

			if (left_open > right_open + deadband)
			{
				moveprobe_accel_strafe_sign[slot] = 1;   // curl left (toward open)
			}
			else if (right_open > left_open + deadband)
			{
				moveprobe_accel_strafe_sign[slot] = -1;  // curl right (toward open)
			}

			if (fixed_angle > 0)
			{
				rotation = fixed_angle;
			}
			else if (hor_speed_sq > max_incr * max_incr)
			{
				rotation = acos(max_incr / sqrt(hor_speed_sq)) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir,
									rotation * moveprobe_accel_strafe_sign[slot]);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		press_jump = onground && !moveprobe_accel_jump_press[slot];
		moveprobe_accel_jump_press[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}

	else if (mode == 23)
	{
		// HYBRID nav-weave: vanilla frogbot navigation decides WHERE (dir_move_ ->
		// the linked marker, set by BotMoveTowardsLinkedMarker), the mode-20/21
		// cs->0 accel law decides HOW FAST. No replay corridor: the bot bunnyhops
		// along its own marker graph, weaving across the marker bearing with the
		// mode-21 hysteresis/corner logic. Water and zero-direction (wait/respawn)
		// frames fall through to vanilla movement so swim/stand logic is unchanged.
		float numerator = cvar("k_fb_moveprobe_accel_numerator");
		float bootstrap_deg = cvar("k_fb_moveprobe_accel_bootstrap_deg");
		float look = cvar("k_fb_moveprobe_s19_wall");
		float swing = cvar("k_fb_moveprobe_s21_swing");
		float turn_thresh = cvar("k_fb_moveprobe_s21_turn");
		float corner_aim = cvar("k_fb_moveprobe_s21_corner_aim");
		float corner_thresh = cvar("k_fb_moveprobe_s21_corner_thresh");
		float sv_maxspeed = cvar("sv_maxspeed");
		float pass_r = cvar("k_fb_moveprobe_s23_pass");
		float wallhug_deg = cvar("k_fb_moveprobe_s23_wallhug");
		// A3 #75 protocol instruments (all default 0 = off = the deployed c5
		// law unchanged; set per-run by the measurement protocol exactly like
		// spawn_origin/fixed_goal):
		float deleg_vh_max = cvar("k_fb_moveprobe_s23_deleg_vh_max");
		float launch_vh = cvar("k_fb_moveprobe_s23_launch_vh");
		float launch_angle = cvar("k_fb_moveprobe_s23_launch_angle");
		float ztarget_x = cvar("k_fb_moveprobe_s23_launch_target_x");
		float ztarget_y = cvar("k_fb_moveprobe_s23_launch_target_y");
		float ztarget_z = cvar("k_fb_moveprobe_s23_launch_target_z");
		float zlip_x = cvar("k_fb_moveprobe_s23_lip_x");
		float zlip_y = cvar("k_fb_moveprobe_s23_lip_y");
		float zrelease_vh = cvar("k_fb_moveprobe_s23_release_vh");
		float zrelease_vh_min = cvar("k_fb_moveprobe_s23_release_vh_min");
		float zcarve_d = cvar("k_fb_moveprobe_s23_carve_d");
		float zcarve_angle = cvar("k_fb_moveprobe_s23_carve_angle");
		int zcarve_side = (int)cvar("k_fb_moveprobe_s23_carve_side");
		float zrelease_lip = cvar("k_fb_moveprobe_s23_release_lip");
		float zrefcurve = cvar("k_fb_moveprobe_s23_refcurve");
		float zrefcurve_vh_min = cvar("k_fb_moveprobe_s23_refcurve_vh_min");
		float zrefcurve_yaw_offset = cvar("k_fb_moveprobe_s23_refcurve_yaw_offset");
		float zrefcurve_entry_x = cvar("k_fb_moveprobe_s23_refcurve_entry_x");
		float zrefcurve_entry_y = cvar("k_fb_moveprobe_s23_refcurve_entry_y");
		float zrefcurve_y = cvar("k_fb_moveprobe_s23_refcurve_y");
		float zrefcurve_y_tol = cvar("k_fb_moveprobe_s23_refcurve_y_tol");
		float zyawlead_min = cvar("k_fb_moveprobe_s23_yawlead_min");
		float zyawlead_max = cvar("k_fb_moveprobe_s23_yawlead_max");
		float ztargeterr_min = cvar("k_fb_moveprobe_s23_targeterr_min");
		float ztargeterr_max = cvar("k_fb_moveprobe_s23_targeterr_max");
		qbool zjump_enabled, deleg_speed_ok;
		qbool ztrack, zterminal, zcorridor, zarmed, zcurve, ztarget_ok, zyawlead_ok;
		float hor_speed_sq, rotation, goal_yaw, vel_yaw, signed_to_goal, herr, corner_mag;
		float zvh, zd_lip, zlip_dx, zlip_dy, zlip_len, ztarget_yaw, ztarget_err, zyaw_lead, ztmp;
		float zdesired_vel_yaw, zdesired_view_yaw, zvel_err, zcorridor_err;
		float marker_dist_sq, marker_dz = 0;
		int sign = 0, zside = 0, zrelease_rule = 0;
		int path_flags = (int)self->fb.path_state;
		qbool onground = ((int)self->s.v.flags & FL_ONGROUND) ? true : false;
		qbool press_jump, hard_corner, pass_through, climb;
		vec3_t nav_dir, cur_dir, proposed_dir, ztarget_dir;
		vec3_t left_dir, right_dir, lp, rp, fp;
		vec3_t up_vector = { 0, 0, 1 };
		float left_open, right_open, fwd_open;

		if (pass_r <= 0) pass_r = 130.0f;
		if (wallhug_deg <= 0) wallhug_deg = 40.0f;
		if (numerator <= 0) numerator = 9.0f;
		if (bootstrap_deg <= 0) bootstrap_deg = 25.0f;
		if (look <= 0) look = 500.0f;
		if (swing <= 0) swing = 12.0f;
		if (turn_thresh <= 0) turn_thresh = 35.0f;
		if (corner_aim <= 0) corner_aim = 68.0f;
		if (corner_thresh <= 0) corner_thresh = 58.0f;
		if (sv_maxspeed <= 0) sv_maxspeed = 320.0f;
		if (launch_angle <= 0) launch_angle = 45.0f;
		// ZTRICKS TERMINAL CARVE (default-off): route metadata can configure a
		// landing target/lip pair so mode 23 deliberately reproduces the final
		// human speedjump "symphony": keep accelerating, then near the lip emit
		// a right/left strafe-jump command whose wishdir, view-yaw lead, speed
		// floor, and jump release are all scored in FBMOVEPROBE_CMD zjump rows.
		// With all target coordinates left at 0, this block is inert and the
		// deployed mode-23 law is unchanged.
		zjump_enabled = ((ztarget_x != 0.0f) || (ztarget_y != 0.0f) || (ztarget_z != 0.0f));
		if (zjump_enabled)
		{
			if (zlip_x == 0.0f) zlip_x = -3348.0f;
			if (zrelease_vh <= 0) zrelease_vh = 470.0f;
			if (zrelease_vh_min <= 0) zrelease_vh_min = 453.0f;
			if (zcarve_d <= 0) zcarve_d = 80.0f;
			if (zcarve_angle <= 0) zcarve_angle = 52.0f;
			if (zrelease_lip <= 0) zrelease_lip = 35.0f;
			if (zrefcurve_vh_min < 0) zrefcurve_vh_min = 0.0f;
			if ((zrefcurve_y != 0.0f) && (zrefcurve_y_tol <= 0.0f))
			{
				zrefcurve_y_tol = 24.0f;
			}
			if ((zrefcurve_entry_x == 0.0f) && (zrefcurve_entry_y == 0.0f))
			{
				zrefcurve_entry_x = -3439.375f;
				zrefcurve_entry_y = 3758.125f;
			}
			if ((zyawlead_min == 0.0f) && (zyawlead_max == 0.0f))
			{
				zyawlead_min = -12.0f;
				zyawlead_max = -4.0f;
			}
			if ((ztargeterr_min == 0.0f) && (ztargeterr_max == 0.0f))
			{
				ztargeterr_min = -2.0f;
				ztargeterr_max = 10.0f;
			}
			if (zyawlead_min > zyawlead_max)
			{
				ztmp = zyawlead_min;
				zyawlead_min = zyawlead_max;
				zyawlead_max = ztmp;
			}
			if (ztargeterr_min > ztargeterr_max)
			{
				ztmp = ztargeterr_min;
				ztargeterr_min = ztargeterr_max;
				ztargeterr_max = ztmp;
			}
		}
		// Delegation speed gate (A3 #75, sim knob deleg_vh_max): the grounded
		// climb delegation exists for SLOW stair legs; on a flying return it
		// would hijack the run and scrub the carried speed. Gate delegation
		// (and the carrot's delegation-exact guard, below) on horizontal
		// speed. Cvar unset (0) = gate inert (always true) = deployed law.
		deleg_speed_ok = (deleg_vh_max <= 0)
			|| ((self->s.v.velocity[0] * self->s.v.velocity[0]
				 + self->s.v.velocity[1] * self->s.v.velocity[1])
				<= deleg_vh_max * deleg_vh_max);

		if (self->s.v.waterlevel > 1)
		{
			return;
		}
		// Per-tick bearing to the linked marker itself (fresher than dir_move_,
		// which updates only at frogbot think rate and carries dodge noise).
		// Falls back to dir_move_ when there is no usable marker.
		if (self->fb.linked_marker && (self->fb.linked_marker != self->fb.touch_marker))
		{
			vec3_t m_org;

			VectorAdd(self->fb.linked_marker->s.v.absmin,
					  self->fb.linked_marker->s.v.view_ofs, m_org);
			VectorSubtract(m_org, self->s.v.origin, nav_dir);
			marker_dist_sq = nav_dir[0] * nav_dir[0] + nav_dir[1] * nav_dir[1];
			marker_dz = nav_dir[2];

			// CARROT (look-through): closing within pass_r of the linked marker
			// counts as touching it -- run the SAME handover a physical touch
			// would (SetMarker + ProcessNewLinkedMarker, frogbot's own selection
			// machinery; do NOT clone its scoring) so the weave aims at the NEXT
			// hop while flying past this one. Edge-triggered per marker so the
			// stochastic path scorer is consulted once per pass.
			if ((marker_dist_sq < pass_r * pass_r)
				&& (moveprobe_s23_carrot_done[slot] != self->fb.linked_marker)
				&& !(onground && (marker_dz > 18.0f)
					 && (marker_dist_sq < 280.0f * 280.0f)
					 && !(path_flags & (JUMP_LEDGE | WATERJUMP_ | ROCKET_JUMP))
					 && deleg_speed_ok))
			{
				// Climb guard mirrors the DELEGATION condition exactly: a
				// handover mid-staircase re-targets past delegation range and
				// re-enables hopping on the stairs. The broader config-2/4 guard
				// (any close climb) also blocked handovers at jump-flagged
				// ledges where delegation never runs -- cost rung reach 8->5/10.
				gedict_t *passed = self->fb.linked_marker;
				float old_bearing, new_bearing, leg_turn;
				vec3_t old_leg2d;

				// Positional bearing of the CURRENT leg (bot->passed marker),
				// captured before the handover. Velocity heading is useless here:
				// under the +/-45 deg weave it is chronically >60 deg off the leg
				// even on straights (config-2 regression: governor fired
				// everywhere, grounded the bot, reach 8/10 -> 4/10).
				VectorCopy(nav_dir, old_leg2d);
				old_leg2d[2] = 0;

				moveprobe_s23_carrot_done[slot] = passed;
				SetMarker(self, passed);
				ProcessNewLinkedMarker(self);

				if (self->fb.linked_marker && (self->fb.linked_marker != passed)
					&& (self->fb.linked_marker != self->fb.touch_marker))
				{
					VectorAdd(self->fb.linked_marker->s.v.absmin,
							  self->fb.linked_marker->s.v.view_ofs, m_org);
					VectorSubtract(m_org, self->s.v.origin, nav_dir);
					marker_dist_sq = nav_dir[0] * nav_dir[0] + nav_dir[1] * nav_dir[1];
					marker_dz = nav_dir[2];

					// PRECISION GOVERNOR: REMOVED after A/B evidence. Both the
					// velocity-based (config-2) and positional leg-bearing
					// (config-3) variants dropped rung reach-rate 8/10 -> 4/10;
					// grounding at corners interacts badly with the marker-basin
					// recovery the weave provides. The corner-overshoot problem
					// from the P2 decomposition stands, but the governor is the
					// wrong tool at this layer. (old_leg2d kept for telemetry use
					// later if needed.)
					(void)old_leg2d;
					(void)old_bearing;
					(void)new_bearing;
					(void)leg_turn;
				}
			}
		}
		else
		{
			VectorCopy(self->fb.dir_move_, nav_dir);
			marker_dist_sq = 1e18f;
		}

		climb = false;
		nav_dir[2] = 0;
		if (VectorNormalize(nav_dir) <= 0)
		{
			if (zjump_enabled)
			{
				float fallback_x = ztarget_x;
				float fallback_y = ztarget_y;

				if ((zrefcurve > 0.0f) && (zrefcurve_y != 0.0f))
				{
					float yerr = self->s.v.origin[1] - zrefcurve_y;
					if (yerr < 0.0f) yerr = -yerr;
					if (yerr > zrefcurve_y_tol)
					{
						fallback_x = zrefcurve_entry_x;
						fallback_y = zrefcurve_entry_y;
					}
				}
				nav_dir[0] = fallback_x - self->s.v.origin[0];
				nav_dir[1] = fallback_y - self->s.v.origin[1];
				nav_dir[2] = 0;
				marker_dist_sq = nav_dir[0] * nav_dir[0] + nav_dir[1] * nav_dir[1];
				if (VectorNormalize(nav_dir) <= 0)
				{
					return;
				}
			}
			else
			{
				return;
			}
		}
		// PASS-THROUGH zone: close to the marker the bearing swings wildly as the
		// bot flies past; reacting to that grounded + re-aimed the bot at EVERY
		// marker (v1 finding: avg 196 vs vanilla 191 -- no compounding). Inside
		// pass_r, ignore corner logic and keep the weave: navigation hands over
		// the next marker on touch.
		pass_through = (marker_dist_sq < pass_r * pass_r);

		// Movement doctrine (user rule): ALWAYS bunnyhop -- except UP stairs and
		// walkable inclines. Implementation: DELEGATE climb legs to VANILLA
		// actuation (return without overriding direction/jumping), because
		// vanilla provably walks stairs perfectly (0 jump inputs, control run)
		// and its organic ledge-jump logic still mounts >18qu crates -- the
		// terrain-probe versions (v4-v6) either made ledges unmountable or
		// leaked hops near marker handovers. Delegation only when GROUNDED:
		// air frames between approach hops keep the weave steering. Links that
		// REQUIRE a jump (ledge/waterjump/RJ) stay with the weave.
		if (onground && (marker_dz > 18.0f) && (marker_dist_sq < 280.0f * 280.0f)
			&& !(path_flags & (JUMP_LEDGE | WATERJUMP_ | ROCKET_JUMP))
			&& deleg_speed_ok)
		{
			// Livelock guard: if delegation has been walking toward the SAME
			// marker for >3s, vanilla cannot make this climb (it needed an
			// organic jump that never fired; observed: 95%-grounded run circling
			// under marker 9 for a full run). Release to the weave -- a hop can
			// mount the ledge -- until the linked marker changes.
			if (moveprobe_s23_deleg_marker[slot] != self->fb.linked_marker)
			{
				moveprobe_s23_deleg_marker[slot] = self->fb.linked_marker;
				moveprobe_s23_deleg_since[slot] = g_globalvars.time;
			}
			if ((g_globalvars.time - moveprobe_s23_deleg_since[slot]) < 3.0f)
			{
				return;
			}
		}
		else
		{
			moveprobe_s23_deleg_marker[slot] = NULL;
		}

		cur_dir[0] = self->s.v.velocity[0];
		cur_dir[1] = self->s.v.velocity[1];
		cur_dir[2] = 0;
		hor_speed_sq = cur_dir[0] * cur_dir[0] + cur_dir[1] * cur_dir[1];
		if (VectorNormalize(cur_dir) <= 0)
		{
			VectorCopy(nav_dir, cur_dir);
		}

		goal_yaw = vectoyaw(nav_dir);
		vel_yaw = vectoyaw(cur_dir);
		signed_to_goal = goal_yaw - vel_yaw;
		while (signed_to_goal > 180.0f) signed_to_goal -= 360.0f;
		while (signed_to_goal < -180.0f) signed_to_goal += 360.0f;
		herr = (signed_to_goal >= 0) ? signed_to_goal : -signed_to_goal;

		moveprobe_s23_zjump_phase[slot] = 0;
		moveprobe_s23_zjump_armed[slot] = 0;
		moveprobe_s23_zjump_release_rule[slot] = 0;
		moveprobe_s23_zjump_d_lip[slot] = 999999.0f;
		moveprobe_s23_zjump_vh[slot] = sqrt(hor_speed_sq);
		moveprobe_s23_zjump_vel_yaw[slot] = vel_yaw;
		moveprobe_s23_zjump_target_yaw[slot] = 0.0f;
		moveprobe_s23_zjump_target_err[slot] = 0.0f;
		moveprobe_s23_zjump_yaw_lead[slot] = 0.0f;

		if (zjump_enabled)
		{
			ztarget_dir[0] = ztarget_x - self->s.v.origin[0];
			ztarget_dir[1] = ztarget_y - self->s.v.origin[1];
			ztarget_dir[2] = 0;
			if (VectorNormalize(ztarget_dir) > 0)
			{
				zvh = sqrt(hor_speed_sq);
				if (zlip_y != 0.0f)
				{
					zlip_dx = ztarget_x - zlip_x;
					zlip_dy = ztarget_y - zlip_y;
					zlip_len = sqrt(zlip_dx * zlip_dx + zlip_dy * zlip_dy);
					if (zlip_len > 0.001f)
					{
						zlip_dx /= zlip_len;
						zlip_dy /= zlip_len;
						zd_lip = ((zlip_x - self->s.v.origin[0]) * zlip_dx)
							+ ((zlip_y - self->s.v.origin[1]) * zlip_dy);
					}
					else
					{
						zd_lip = zlip_x - self->s.v.origin[0];
					}
				}
				else
				{
					zd_lip = zlip_x - self->s.v.origin[0];
				}
				ztarget_yaw = vectoyaw(ztarget_dir);
				ztarget_err = ztarget_yaw - vel_yaw;
				while (ztarget_err > 180.0f) ztarget_err -= 360.0f;
				while (ztarget_err < -180.0f) ztarget_err += 360.0f;
				ztrack = (zd_lip >= 0.0f) && (zd_lip <= zcarve_d);
				zcorridor = true;
				if (zrefcurve_y != 0.0f)
				{
					zcorridor_err = self->s.v.origin[1] - zrefcurve_y;
					if (zcorridor_err < 0.0f) zcorridor_err = -zcorridor_err;
					zcorridor = (zcorridor_err <= zrefcurve_y_tol);
				}
				zterminal = ztrack && ((zrefcurve <= 0.0f) || zcorridor);
				zarmed = zterminal && (zvh >= zrelease_vh_min);
				zcurve = zterminal && (zrefcurve > 0.0f) && (zvh >= zrefcurve_vh_min);
				zside = zcarve_side;
				if (zside == 0)
				{
					zside = (ztarget_err <= 0.0f) ? 1 : -1;
				}
				zside = (zside >= 0) ? 1 : -1;
				if (zcurve)
				{
					BotMoveProbeZtricksReferenceCurve(zd_lip, &zdesired_vel_yaw,
													  &zdesired_view_yaw);
					zdesired_vel_yaw = anglemod(zdesired_vel_yaw + zrefcurve_yaw_offset);
					zdesired_view_yaw = anglemod(zdesired_view_yaw + zrefcurve_yaw_offset);
					zvel_err = BotMoveProbeWrap180(zdesired_vel_yaw - vel_yaw);
					if (zcarve_side == 0)
					{
						zside = (zvel_err >= 0.0f) ? 1 : -1;
					}
					zyaw_lead = BotMoveProbeWrap180(zdesired_view_yaw - vel_yaw);
				}
				else
				{
					// Positive sidemove is the ztricks right-carve. With fwd+side
					// held equal, a 52 degree wishdir offset means view yaw trails
					// velocity by roughly 7 degrees, matching the human release.
					zyaw_lead = (45.0f - zcarve_angle) * zside;
					if (zyaw_lead < zyawlead_min) zyaw_lead = zyawlead_min;
					if (zyaw_lead > zyawlead_max) zyaw_lead = zyawlead_max;
				}

				moveprobe_s23_zjump_phase[slot] = zterminal ? 1 : 0;
				moveprobe_s23_zjump_armed[slot] = zarmed ? 1 : 0;
				moveprobe_s23_zjump_d_lip[slot] = zd_lip;
				moveprobe_s23_zjump_vh[slot] = zvh;
				moveprobe_s23_zjump_vel_yaw[slot] = vel_yaw;
				moveprobe_s23_zjump_target_yaw[slot] = ztarget_yaw;
				moveprobe_s23_zjump_target_err[slot] = ztarget_err;
				moveprobe_s23_zjump_yaw_lead[slot] = zyaw_lead;

				if (zarmed || zcurve)
				{
					ztarget_ok = (ztarget_err >= ztargeterr_min)
						&& (ztarget_err <= ztargeterr_max);
					zyawlead_ok = (zyaw_lead >= zyawlead_min)
						&& (zyaw_lead <= zyawlead_max);
					if (zarmed && onground && (zvh >= zrelease_vh) && (zd_lip <= zrelease_lip)
						&& ztarget_ok && zyawlead_ok)
					{
						zrelease_rule = 1;
					}
					else if (zarmed && onground && (zd_lip <= zrelease_lip * 0.35f)
							 && (ztarget_err >= (ztargeterr_min - 20.0f))
							 && (ztarget_err <= (ztargeterr_max + 10.0f))
							 && zyawlead_ok)
					{
						zrelease_rule = 2;
					}

					self->fb.desired_angle[PITCH] = 0;
					self->fb.desired_angle[YAW] = anglemod(vel_yaw + zyaw_lead);
					self->fb.desired_angle[ROLL] = 0;
					direction[0] = sv_maxspeed;
					direction[1] = sv_maxspeed * zside;
					direction[2] = 0;
					press_jump = onground && !moveprobe_accel_jump_press[slot];
					*jumping = zrelease_rule ? true : (!zarmed && zcurve
						&& (zd_lip <= zrelease_lip) && press_jump);
					moveprobe_accel_jump_press[slot] = *jumping;
					moveprobe_s23_zjump_phase[slot] = zrelease_rule ? 2 : 1;
					moveprobe_s23_zjump_release_rule[slot] = zrelease_rule;
					if (zrelease_rule)
					{
						moveprobe_s23_launch_done[slot] = 1;
					}
					return;
				}
			}
		}

		// CIRCLE-JUMP LAUNCH (A2b #111 -> A3 #75): one-shot at attempt start.
		// The >=526 tail tries all entered the final runway already fast via
		// an ACCIDENTAL grounded circle-strafe; this does it deliberately,
		// once. While grounded below launch_vh: suppress the jump and hold
		// the wishdir launch_angle deg off the velocity (the grounded circle)
		// -- QW ground accelerate keeps adding speed past maxspeed while the
		// velocity projection onto the wishdir stays under it. Release into
		// the hop chain when fast (>= launch_vh) AND aimed (|signed_to_goal|
		// <= swing), or at the 3 s safeguard. Engage gate: the LOOK ray
		// toward the nav direction must be >= 0.9 open (a human does not
		// circle-jump into a wall). Measured limitation (A2b): the ray
		// filters direct-wall starts only -- it cannot size the circle's ARC
		// room (tight rooms wall-lock), so the launch cvars are PER-PROTOCOL
		// instruments like spawn_origin/fixed_goal: default 0 in general
		// play = this block never runs.
		if ((launch_vh > 0) && !moveprobe_s23_launch_done[slot])
		{
			if (moveprobe_s23_launch_since[slot] <= 0)
			{
				VectorMA(self->s.v.origin, look, nav_dir, fp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(fp), false, self);
				if (g_globalvars.trace_fraction < 0.9f)
				{
					moveprobe_s23_launch_done[slot] = 1;   // no runway: unmodified law
				}
				moveprobe_s23_launch_since[slot] = g_globalvars.time;
			}
			if (moveprobe_s23_launch_done[slot])
			{
				// engage refused this attempt -- unmodified law from here on
			}
			else if ((g_globalvars.time - moveprobe_s23_launch_since[slot]) >= 3.0f)
			{
				moveprobe_s23_launch_done[slot] = 1;       // safeguard release
			}
			else if (onground)
			{
				if ((hor_speed_sq >= launch_vh * launch_vh) && (herr <= swing))
				{
					moveprobe_s23_launch_done[slot] = 1;   // fast + aimed: release (the jump fires below)
				}
				else
				{
					// the grounded circle: full forward along a wishdir held
					// launch_angle off the velocity, jump suppressed
					if (moveprobe_accel_strafe_sign[slot] == 0)
					{
						moveprobe_accel_strafe_sign[slot] = (signed_to_goal >= 0.0f) ? 1 : -1;
					}
					RotatePointAroundVector(proposed_dir, up_vector, cur_dir,
											launch_angle * moveprobe_accel_strafe_sign[slot]);
					proposed_dir[2] = 0;
					if (VectorNormalize(proposed_dir) <= 0)
					{
						VectorCopy(cur_dir, proposed_dir);
					}
					moveprobe_accel_jump_press[slot] = false;
					self->fb.desired_angle[PITCH] = 0;
					self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
					self->fb.desired_angle[ROLL] = 0;
					direction[0] = sv_maxspeed;
					direction[1] = 0;
					direction[2] = 0;
					*jumping = false;
					return;
				}
			}
		}

		if (onground)
		{
			// Ground frame: aim straight at the marker (full ground redirect --
			// also the human cornering technique on sharp bends).
			VectorCopy(nav_dir, proposed_dir);

			// Wall-hug (user doctrine): when forced to STAY grounded (a climb), lean
			// the wishdir into a nearby wall. The wall clips velocity along itself
			// and ground speed equilibrium rises toward 320/cos(angle) minus
			// friction (~400 qu/s on a long wall) -- the human grounded-speed
			// technique. MUST NOT fire on bunnyhop touch frames (v3 regression:
			// rotating every hop's ground-accel frame 40 deg off scrubbed avg
			// speed 227->150 map-wide).
			if (climb && (herr <= turn_thresh))
			{
				vec3_t side_dir, side_p;
				float l_open, r_open;

				RotatePointAroundVector(side_dir, up_vector, nav_dir, 90.0f);
				VectorMA(self->s.v.origin, 50.0f, side_dir, side_p);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(side_p), false, self);
				l_open = g_globalvars.trace_fraction;
				RotatePointAroundVector(side_dir, up_vector, nav_dir, -90.0f);
				VectorMA(self->s.v.origin, 50.0f, side_dir, side_p);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(side_p), false, self);
				r_open = g_globalvars.trace_fraction;

				if ((l_open < 1.0f) && (l_open <= r_open))
				{
					RotatePointAroundVector(proposed_dir, up_vector, nav_dir, wallhug_deg);
				}
				else if (r_open < 1.0f)
				{
					RotatePointAroundVector(proposed_dir, up_vector, nav_dir, -wallhug_deg);
				}
				proposed_dir[2] = 0;
				if (VectorNormalize(proposed_dir) <= 0)
				{
					VectorCopy(nav_dir, proposed_dir);
				}
			}
		}
		else
		{
			hard_corner = false;
			if (herr > turn_thresh && !pass_through)
			{
				sign = (signed_to_goal >= 0.0f) ? 1 : -1;
				moveprobe_accel_strafe_sign[slot] = sign;
				if (herr > corner_thresh)
				{
					hard_corner = true;
				}
			}
			else if (pass_through)
			{
				// Flying past the marker: HOLD the current curl, no flips, no
				// corner reaction -- carry speed through the handover.
				if (moveprobe_accel_strafe_sign[slot] == 0)
					moveprobe_accel_strafe_sign[slot] = 1;
				sign = moveprobe_accel_strafe_sign[slot];
			}
			else
			{
				if (moveprobe_accel_strafe_sign[slot] == 0)
					moveprobe_accel_strafe_sign[slot] = (signed_to_goal >= 0.0f) ? 1 : -1;
				if (moveprobe_accel_strafe_sign[slot] > 0 && signed_to_goal < -swing)
					moveprobe_accel_strafe_sign[slot] = -1;
				else if (moveprobe_accel_strafe_sign[slot] < 0 && signed_to_goal > swing)
					moveprobe_accel_strafe_sign[slot] = 1;
				sign = moveprobe_accel_strafe_sign[slot];
			}

			// Wall safety net: if velocity runs into a near wall, curl to the open side.
			VectorMA(self->s.v.origin, look, cur_dir, fp);
			traceline(PASSVEC3(self->s.v.origin), PASSVEC3(fp), false, self);
			fwd_open = g_globalvars.trace_fraction;
			if (fwd_open < 0.35f)
			{
				RotatePointAroundVector(left_dir, up_vector, cur_dir, 45.0f);
				RotatePointAroundVector(right_dir, up_vector, cur_dir, -45.0f);
				VectorMA(self->s.v.origin, look, left_dir, lp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(lp), false, self);
				left_open = g_globalvars.trace_fraction;
				VectorMA(self->s.v.origin, look, right_dir, rp);
				traceline(PASSVEC3(self->s.v.origin), PASSVEC3(rp), false, self);
				right_open = g_globalvars.trace_fraction;
				sign = (left_open >= right_open) ? 1 : -1;
				moveprobe_accel_strafe_sign[slot] = sign;
				hard_corner = false;
			}

			if (hard_corner)
			{
				corner_mag = herr;
				if (corner_mag > corner_aim)
				{
					corner_mag = corner_aim;
				}
				rotation = corner_mag;
			}
			else if (hor_speed_sq > numerator * numerator)
			{
				rotation = acos(numerator / sqrt(hor_speed_sq)) * 180.0f / M_PI;
			}
			else
			{
				rotation = bootstrap_deg;
			}
			RotatePointAroundVector(proposed_dir, up_vector, cur_dir, rotation * sign);
			proposed_dir[2] = 0;
			if (VectorNormalize(proposed_dir) <= 0)
			{
				VectorCopy(cur_dir, proposed_dir);
			}
		}

		// Precision leg (sharp corner per the governor): override the weave --
		// aim straight at the marker; grounded frames redirect + shed speed.
		// Cleared when the marker is taken (linked changes) or after 2s escape.
		if ((moveprobe_s23_prec_marker[slot] != NULL)
			&& (moveprobe_s23_prec_marker[slot] == self->fb.linked_marker)
			&& ((g_globalvars.time - moveprobe_s23_prec_since[slot]) < 2.0f))
		{
			VectorCopy(nav_dir, proposed_dir);
		}
		else
		{
			moveprobe_s23_prec_marker[slot] = NULL;
		}

		// Climb (stairs/incline/lift): never hop -- walk it with ground accel +
		// wall-hug. Sharp bend: stay grounded so ground accel can redirect
		// velocity hard. Otherwise toggle-hop (the bunnyhop default).
		// Marker-pass transients (pass_through) never ground on bends -- that was
		// v1's speed killer -- but climb and precision legs win over pass_through.
		if ((moveprobe_s23_prec_marker[slot] != NULL) || climb
			|| (herr > turn_thresh && !pass_through))
		{
			press_jump = false;
		}
		else
		{
			press_jump = onground && !moveprobe_accel_jump_press[slot];
		}
		moveprobe_accel_jump_press[slot] = press_jump;

		self->fb.desired_angle[PITCH] = 0;
		self->fb.desired_angle[YAW] = vectoyaw(proposed_dir);
		self->fb.desired_angle[ROLL] = 0;
		direction[0] = sv_maxspeed;
		direction[1] = 0;
		direction[2] = 0;
		*jumping = press_jump;
	}
}

static float BotMoveProbeAngleDelta(float route_yaw, float view_yaw)
{
	float delta = anglemod(route_yaw) - anglemod(view_yaw);

	if (delta > 180)
	{
		delta -= 360;
	}
	else if (delta < -180)
	{
		delta += 360;
	}

	return delta;
}

static int BotMoveProbeMarkerIndex(gedict_t *marker)
{
	return marker ? marker->fb.index + 1 : -1;
}

static void BotLogMoveProbeCommand(gedict_t *self, int cmd_msec, vec3_t direction,
								   int buttons, int impulse)
{
	// Probe-only: current lab runs start fresh KTX processes, so this throttle
	// state is zeroed. Reset it before reusing the logger in a long-lived server.
	static float last_log_time[MAX_CLIENTS];
	int ednum = NUM_FOR_EDICT(self);
	int slot = ednum - 1;
	int mode;
	float interval;
	float route_yaw = 0;
	float view_yaw = anglemod(self->fb.desired_angle[YAW]);
	float yaw_delta = 0;
	int backward = direction[0] < 0;
	vec3_t route_direction;
	int goalentity = self->s.v.goalentity;
	int goal_marker = -1;
	int blocked = 0;

	if (!cvar("k_fb_moveprobe_log_commands") || slot < 0 || slot >= MAX_CLIENTS)
	{
		return;
	}

	interval = cvar("k_fb_moveprobe_log_interval");
	if (interval < 0)
	{
		interval = 0;
	}

	if ((interval > 0) && last_log_time[slot]
		&& ((g_globalvars.time - last_log_time[slot]) < interval))
	{
		return;
	}

	last_log_time[slot] = g_globalvars.time;
	{
		int mode_from_slot;

		if (BotMoveProbeCvarIntForBot(self, "mode", &mode, &mode_from_slot) < 0)
		{
			mode = -1; // held by the per-slot loud-fail contract; mark the row
		}
	}

	VectorCopy(self->fb.dir_move_, route_direction);
	route_direction[2] = 0;
	if (VectorNormalize(route_direction) > 0)
	{
		route_yaw = anglemod(vectoyaw(route_direction));
		// yaw_delta is interpretable for aim-independent modes 5/6/7. Route-yaw
		// modes set view yaw from route yaw, so their deltas are structural noise.
		yaw_delta = BotMoveProbeAngleDelta(route_yaw, view_yaw);
	}

	if ((goalentity > 0) && (goalentity < MAX_EDICTS))
	{
		goal_marker = BotMoveProbeMarkerIndex(g_edicts[goalentity].fb.touch_marker);
	}

	blocked = (self->fb.path_state & STUCK_PATH)
		|| self->fb.obstruction_normal[0]
		|| self->fb.obstruction_normal[1]
		|| self->fb.obstruction_normal[2];

	if (mode != 23)
	{
		moveprobe_s23_zjump_phase[slot] = 0;
		moveprobe_s23_zjump_armed[slot] = 0;
		moveprobe_s23_zjump_release_rule[slot] = 0;
		moveprobe_s23_zjump_d_lip[slot] = 999999.0f;
		moveprobe_s23_zjump_vh[slot] = 0.0f;
		moveprobe_s23_zjump_vel_yaw[slot] = 0.0f;
		moveprobe_s23_zjump_target_yaw[slot] = 0.0f;
		moveprobe_s23_zjump_target_err[slot] = 0.0f;
		moveprobe_s23_zjump_yaw_lead[slot] = 0.0f;
	}
	if (mode != 25)
	{
		moveprobe_s25_active[slot] = 0;
		moveprobe_s25_engaged[slot] = 0;
		moveprobe_s25_reason[slot] = 0;
		moveprobe_s25_sign[slot] = 0;
		moveprobe_s25_hs[slot] = 0.0f;
		moveprobe_s25_target_hs[slot] = 0.0f;
		moveprobe_s25_speed_gap[slot] = 0.0f;
		moveprobe_s25_rotation[slot] = 0.0f;
		moveprobe_s25_wish_yaw[slot] = 0.0f;
		moveprobe_s25_vel_yaw[slot] = 0.0f;
		moveprobe_s25_target_vel_yaw[slot] = 0.0f;
		moveprobe_s25_target_vel_err[slot] = 0.0f;
		moveprobe_s25_out_fwd[slot] = 0.0f;
		moveprobe_s25_out_side[slot] = 0.0f;
	}

	G_cprint("FBMOVEPROBE_CMD time=%.3f ed=%d name=%s mode=%d msec=%d "
			 "angles=%.1f,%.1f,%.1f move=%d,%d,%d buttons=%d impulse=%d "
			 "diag=%.1f,%.1f,%.1f,%d "
			 "route=%d,%d,%d,%d,%d,%d,%d,%.3f "
			 "water=%d,%d,%d,%d,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,%.3f "
			 "probe=%d,%d,%.3f,%.3f,%.3f "
			 "qwd=%d,%d,%d,%.3f,%d,%d,%.3f "
			 "replay=%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f "
			 "origin=%.3f,%.3f,%.3f "
			 "zjump=%d,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%d,%d "
			 "s25=%d,%d,%d,%.1f,%.1f,%.1f,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
			 g_globalvars.time, ednum, self->netname, mode, cmd_msec,
			 PASSVEC3(self->fb.desired_angle), PASSINTVEC3(direction),
			 buttons, impulse, route_yaw, view_yaw, yaw_delta, backward,
			 BotMoveProbeMarkerIndex(self->fb.linked_marker),
			 BotMoveProbeMarkerIndex(self->fb.touch_marker), goalentity, goal_marker,
			 self->fb.path_state, self->fb.state, blocked, self->fb.dir_speed,
			 (int)self->s.v.waterlevel, (int)self->s.v.watertype, (int)self->s.v.flags,
			 (int)self->fb.swim_arrow, direction[2],
			 PASSVEC3(self->s.v.velocity), PASSVEC3(self->fb.dir_move_),
			 moveprobe_transition_active[slot], moveprobe_transition_on_ground[slot],
			 moveprobe_transition_since_ground[slot], moveprobe_transition_since_air[slot],
			 moveprobe_transition_scale_used[slot],
			 moveprobe_qwd_active[slot], moveprobe_qwd_point_index[slot],
			 moveprobe_qwd_point_count[slot], moveprobe_qwd_distance[slot],
			 moveprobe_qwd_advanced_points[slot], moveprobe_qwd_complete[slot],
			 moveprobe_qwd_active_seconds[slot],
			 moveprobe_replay_active[slot], moveprobe_replay_complete[slot],
			 moveprobe_replay_cursor[slot], BotMoveProbeReplayCountForSlot(slot),
			 moveprobe_replay_divergence[slot], PASSVEC3(moveprobe_replay_expected[slot]),
			 moveprobe_replay_divergence_h[slot], moveprobe_replay_divergence_v[slot],
			 PASSVEC3(self->s.v.origin),
			 moveprobe_s23_zjump_phase[slot], moveprobe_s23_zjump_d_lip[slot],
			 moveprobe_s23_zjump_vh[slot], moveprobe_s23_zjump_vel_yaw[slot],
			 moveprobe_s23_zjump_target_yaw[slot], moveprobe_s23_zjump_target_err[slot],
			 moveprobe_s23_zjump_yaw_lead[slot], moveprobe_s23_zjump_armed[slot],
			 moveprobe_s23_zjump_release_rule[slot],
			 moveprobe_s25_active[slot], moveprobe_s25_engaged[slot],
			 moveprobe_s25_reason[slot], moveprobe_s25_hs[slot],
			 moveprobe_s25_target_hs[slot], moveprobe_s25_speed_gap[slot],
			 moveprobe_s25_sign[slot], moveprobe_s25_rotation[slot],
			 moveprobe_s25_wish_yaw[slot], moveprobe_s25_vel_yaw[slot],
			 moveprobe_s25_target_vel_yaw[slot], moveprobe_s25_target_vel_err[slot],
			 moveprobe_s25_out_fwd[slot], moveprobe_s25_out_side[slot]);
}

void BotSetCommand(gedict_t *self)
{
	extern float last_frame_time;
	float msec_since_last = (last_frame_time - self->fb.last_cmd_sent) * 1000;
	int cmd_msec = (int)msec_since_last;
	int weapon_script_impulse = 0;
	int impulse = 0, buttons = 0;
	int slot = NUM_FOR_EDICT(self) - 1;
	qbool jumping;
	qbool firing;
	vec3_t direction;

	BotPerformRocketJump(self);

	if (cmd_msec)
	{
		self->fb.cmd_msec_lost += (msec_since_last - cmd_msec);
		if (self->fb.cmd_msec_lost >= 1)
		{
			self->fb.cmd_msec_lost -= 1;
			cmd_msec += 1;
		}
	}
	else if (self->fb.cmd_msec_last)
	{
		// Probably re-sending after blocked(), re-use old number
		cmd_msec = self->fb.cmd_msec_last;
	}
	else
	{
		cmd_msec = 12;
	}

	//G_sprint(self, PRINT_HIGH, "Movement length @ %f: %d\n", last_frame_time, cmd_msec);

	// dir_move_ is the direction we want to move in, but need to take inertia into effect
	// ... as rough guide (and save doubling physics calculations), scale command >
	VectorNormalize(self->fb.dir_move_);
	VectorScale(self->fb.dir_move_, sv_maxspeed, self->fb.last_cmd_direction);

	trap_makevectors(self->fb.desired_angle);

	// During intermission, always do nothing and leave humans to change level
	if (intermission_running)
	{
		self->fb.firing = self->fb.jumping = false;
	}
	else if (teamplay && (deathmatch == 1) && !self->fb.firing)
	{
		// Weaponscripts
		if ((self->s.v.weapon != IT_SHOTGUN) && (self->s.v.weapon != IT_AXE))
		{
			weapon_script_impulse = (self->s.v.ammo_shells ? 2 : 1);
		}
	}

	impulse = self->fb.botchose ? self->fb.next_impulse :
				self->fb.firing ? self->fb.desired_weapon_impulse : weapon_script_impulse;

	if (self->fb.firing && BotUsingCorrectWeapon(self))
	{
		impulse = 0; // we already have the requested weapon
	}

	jumping = self->fb.jumping || self->fb.waterjumping;
	firing = self->fb.firing;

	self->fb.waterjumping = false;

	if (self->fb.dbg_countdown > 0)
	{
		jumping = firing = false;
		VectorClear(direction);
		--self->fb.dbg_countdown;
	}
	else
	{
		if (jumping && ((int)self->s.v.flags & FL_ONGROUND))
		{
			BestJumpingDirection(self);
		}
		else
		{
			ApplyPhysics(self);
		}

		if (self->s.v.waterlevel <= 1)
		{
			vec3_t hor;

			VectorCopy(self->fb.dir_move_, hor);
			hor[2] = 0;
			VectorNormalize(hor);
			VectorScale(hor, 800, hor);

			direction[0] = DotProduct(g_globalvars.v_forward, hor);
			direction[1] = DotProduct(g_globalvars.v_right, hor);
			direction[2] = 0;
		}
		else
		{
			direction[0] = DotProduct (g_globalvars.v_forward, self->fb.dir_move_) * 800;
			direction[1] = DotProduct (g_globalvars.v_right, self->fb.dir_move_) * 800;
			direction[2] = DotProduct (g_globalvars.v_up, self->fb.dir_move_) * 800;
		}

#ifdef DEBUG_MOVEMENT
		if (self->fb.debug_path) {
			G_bprint (PRINT_HIGH, "     : final direction sent [%4.1f %4.1f %4.1f]\n", PASSVEC3 (self->fb.dir_move_));
		}
#endif
	}

	self->fb.desired_angle[2] = 0;

	if (ISDEAD(self))
	{
		firing = false;
		jumping = BotRequestRespawn(self);
		VectorClear(direction);
		impulse = 0;
	}
	else if (self->fb.min_move_time > g_globalvars.time)
	{
		VectorClear(direction);
	}

	// Keep bots on spawns before match start
	if (match_in_progress != 2 && cvar(FB_CVAR_FREEZE_PREWAR))
	{
		jumping = firing = false;
		VectorClear(direction);
		impulse = 0;
	}

	BotApplyMoveProbe(self, &jumping, &firing, &impulse, direction);

	if ((slot >= 0) && (slot < MAX_CLIENTS) && (moveprobe_replay_cmd_msec[slot] > 0))
	{
		cmd_msec = moveprobe_replay_cmd_msec[slot];
	}

	buttons |= (firing ? 1 : 0);
	buttons |= (jumping ? 2 : 0);
	BotLogMoveProbeCommand(self, cmd_msec, direction, buttons, impulse);
	trap_SetBotCMD(NUM_FOR_EDICT(self), cmd_msec, PASSVEC3(self->fb.desired_angle),
					PASSVEC3(direction), buttons, impulse);

	self->fb.next_impulse = 0;
	self->fb.botchose = false;
	self->fb.last_cmd_sent = last_frame_time;
	self->fb.cmd_msec_last = cmd_msec;

	VectorClear(self->fb.obstruction_normal);
	if (self->s.v.button0 && !firing)
	{
		// Stopped firing, randomise next time
		self->fb.last_rndaim_time = 0;
	}

	self->fb.prev_look_object = self->fb.look_object;
	VectorCopy(self->s.v.velocity, self->fb.prev_velocity);
}

#endif
