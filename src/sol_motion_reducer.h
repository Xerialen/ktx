#ifndef SOL_MOTION_REDUCER_H
#define SOL_MOTION_REDUCER_H

#include <stddef.h>
#include <stdint.h>

enum {
	SOL_MOTION_PROGRESS_UNITS_V1 = 8,
	SOL_MOTION_ESCAPE_UPMOVE_V1 = 400
};

typedef enum sol_motion_status_v1 {
	SOL_MOTION_OK_V1 = 1,
	SOL_MOTION_BAD_ARGUMENT_V1 = -1,
	SOL_MOTION_BAD_CONFIG_V1 = -2,
	SOL_MOTION_BAD_SAMPLE_V1 = -3,
	SOL_MOTION_SEQUENCE_ERROR_V1 = -4,
	SOL_MOTION_BAD_STATE_V1 = -5
} sol_motion_status_v1;

typedef enum sol_motion_phase_v1 {
	SOL_MOTION_SUSPENDED_V1 = 0,
	SOL_MOTION_TRACKING_V1 = 1,
	SOL_MOTION_RECOVERING_V1 = 2
} sol_motion_phase_v1;

enum sol_motion_event_v1 {
	SOL_MOTION_PROGRESS_V1 = UINT32_C(1) << 0,
	SOL_MOTION_STUCK_STARTED_V1 = UINT32_C(1) << 1,
	SOL_MOTION_REPLAN_STARTED_V1 = UINT32_C(1) << 2,
	SOL_MOTION_STUCK_CLEARED_V1 = UINT32_C(1) << 3
};

typedef struct sol_motion_config_v1 {
	uint32_t struct_size;
	uint32_t stuck_replan_ms;
} sol_motion_config_v1;

/* Epoch zero is an intentional hold. A strategic goal change uses a fresh
 * nonzero epoch; ordinary steering changes may retain the current epoch. */
typedef struct sol_motion_intent_v1 {
	uint64_t epoch;
	int16_t forwardmove;
	int16_t sidemove;
	int16_t upmove;
} sol_motion_intent_v1;

typedef struct sol_motion_sample_v1 {
	uint64_t frame_seq;
	uint32_t dt_us;
	int16_t origin[3];
	int16_t velocity[3];
	uint8_t can_move;
	sol_motion_intent_v1 intent;
} sol_motion_sample_v1;

typedef struct sol_motion_result_v1 {
	uint64_t no_progress_us;
	uint32_t episode_index;
	uint32_t event_flags;
	uint8_t phase;
	uint8_t recovery_attempt;
	int16_t forwardmove;
	int16_t sidemove;
	int16_t upmove;
} sol_motion_result_v1;

/* Caller-owned, per-seat reducer state. Only sol_motion_init_v1 and
 * sol_motion_step_v1 may mutate it. */
typedef struct sol_motion_state_v1 {
	uint64_t threshold_us;
	uint64_t last_frame_seq;
	uint64_t no_progress_us;
	uint64_t intent_epoch;
	uint64_t recovery_bucket;
	uint32_t prior_dt_us;
	uint32_t episode_index;
	int16_t anchor[3];
	uint8_t initialized;
	uint8_t have_frame;
	uint8_t prior_active;
	uint8_t recovering;
} sol_motion_state_v1;

sol_motion_status_v1 sol_motion_init_v1(sol_motion_state_v1 *state,
	const sol_motion_config_v1 *config);

/* dt_us describes the command interval beginning at this observation. The
 * next contiguous observation proves whether that completed interval made
 * progress, so reduction charges the prior sample's duration. */
sol_motion_status_v1 sol_motion_step_v1(sol_motion_state_v1 *state,
	const sol_motion_sample_v1 *sample, sol_motion_result_v1 *result);

#endif
