#ifndef SOL_KTX_ADAPTER_H
#define SOL_KTX_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "sol_core.h"

enum
{
	SOL_KTX_COMMAND_V1_SIZE = 25
};

typedef struct sol_ktx_snapshot_v1
{
	uint8_t alive;
	uint8_t on_ground;
	uint8_t water_level;
	uint8_t movement_mode;
	float origin[3];
	float velocity[3];
	float view[3];
} sol_ktx_snapshot_v1;

typedef struct sol_ktx_command_v1
{
	uint8_t msec;
	float angles[3];
	int16_t forwardmove;
	int16_t sidemove;
	int16_t upmove;
	uint8_t buttons;
	uint8_t impulse;
} sol_ktx_command_v1;

int sol_ktx_encode_init_v1(const uint8_t asset_id[32], const uint8_t sensory_id[32],
		const uint8_t goal_id[32], const float goal[3], float radius,
		uint8_t output[SOL_CORE_INIT_V1_SIZE]);

/* A null snapshot deliberately encodes a fresh locked/dead neutral frame. */
int sol_ktx_encode_observation_v1(uint64_t frame_seq, uint32_t dt_us,
		const uint8_t asset_id[32], const uint8_t sensory_id[32],
		const sol_ktx_snapshot_v1 *snapshot,
		uint8_t output[SOL_CORE_OBSERVATION_V1_SIZE]);

int sol_ktx_decode_action_v1(const uint8_t *action, size_t action_size,
		uint64_t expected_frame_seq, uint8_t msec, sol_ktx_command_v1 *output);

int sol_ktx_encode_command_v1(const sol_ktx_command_v1 *command,
		uint8_t output[SOL_KTX_COMMAND_V1_SIZE]);

/* The diagnostic plan seat, evidence seat id, and legacy skill token are distinct. */
int sol_ktx_plan_seat_v1(const char *plan_seat, char *evidence_seat, size_t capacity);
int sol_ktx_add_shape_v1(const char *skill_token, const char *team);

#endif
