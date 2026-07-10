#include "sol_ktx_adapter.h"

#include <string.h>

#define SOL_INIT_GOAL_OFFSET 100
#define SOL_INIT_RADIUS_OFFSET 112

#define SOL_OBSERVATION_ALIVE_OFFSET 80
#define SOL_OBSERVATION_ORIGIN_OFFSET 84
#define SOL_OBSERVATION_VELOCITY_OFFSET 96
#define SOL_OBSERVATION_VIEW_OFFSET 108
#define SOL_OBSERVATION_SIGHT_COUNT_OFFSET 120

#define SOL_ACTION_VIEW_OFFSET 12
#define SOL_ACTION_FORWARD_OFFSET 24
#define SOL_ACTION_BUTTONS_OFFSET 30
#define SOL_ACTION_WEAPON_OFFSET 31
#define SOL_ACTION_TEAMSAY_OFFSET 32

static uint16_t read_u16_le(const uint8_t *bytes)
{
	return (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
	return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8)
			| ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *bytes)
{
	return (uint64_t) read_u32_le(bytes) | ((uint64_t) read_u32_le(bytes + 4) << 32);
}

static void write_u16_le(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t) value;
	bytes[1] = (uint8_t) (value >> 8);
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t) value;
	bytes[1] = (uint8_t) (value >> 8);
	bytes[2] = (uint8_t) (value >> 16);
	bytes[3] = (uint8_t) (value >> 24);
}

static void write_u64_le(uint8_t *bytes, uint64_t value)
{
	write_u32_le(bytes, (uint32_t) value);
	write_u32_le(bytes + 4, (uint32_t) (value >> 32));
}

static int canonical_f32(float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000)
			&& bits != UINT32_C(0x80000000);
}

static int read_f32(const uint8_t *bytes, float *value)
{
	uint32_t bits = read_u32_le(bytes);

	memcpy(value, &bits, sizeof(*value));
	return canonical_f32(*value);
}

static void write_f32(uint8_t *bytes, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	write_u32_le(bytes, bits);
}

static int nonzero_identity(const uint8_t identity[32])
{
	size_t index;
	uint8_t any = 0;

	for (index = 0; index < 32; ++index)
	{
		any |= identity[index];
	}
	return any != 0;
}

static int canonical_vector(const float value[3])
{
	return canonical_f32(value[0]) && canonical_f32(value[1]) && canonical_f32(value[2]);
}

static int canonical_view(const float view[3])
{
	return canonical_vector(view) && view[0] >= -90.0f && view[0] <= 90.0f
			&& view[1] >= -180.0f && view[1] < 180.0f
			&& view[2] >= -180.0f && view[2] < 180.0f;
}

static void write_vector(uint8_t *bytes, const float value[3])
{
	write_f32(bytes, value[0]);
	write_f32(bytes + 4, value[1]);
	write_f32(bytes + 8, value[2]);
}

int sol_ktx_encode_init_v1(const uint8_t asset_id[32], const uint8_t sensory_id[32],
		const uint8_t goal_id[32], const float goal[3], float radius,
		uint8_t output[SOL_CORE_INIT_V1_SIZE])
{
	if (!asset_id || !sensory_id || !goal_id || !goal || !output
			|| !nonzero_identity(asset_id) || !nonzero_identity(sensory_id)
			|| !nonzero_identity(goal_id) || !canonical_vector(goal)
			|| !canonical_f32(radius) || radius <= 0.0f)
	{
		return 0;
	}
	memcpy(output, "SLI1", 4);
	memcpy(output + 4, asset_id, 32);
	memcpy(output + 36, sensory_id, 32);
	memcpy(output + 68, goal_id, 32);
	write_vector(output + SOL_INIT_GOAL_OFFSET, goal);
	write_f32(output + SOL_INIT_RADIUS_OFFSET, radius);
	return 1;
}

int sol_ktx_encode_observation_v1(uint64_t frame_seq, uint32_t dt_us,
		const uint8_t asset_id[32], const uint8_t sensory_id[32],
		const sol_ktx_snapshot_v1 *snapshot,
		uint8_t output[SOL_CORE_OBSERVATION_V1_SIZE])
{
	static const float zero_vector[3] = { 0.0f, 0.0f, 0.0f };
	const float *origin = zero_vector;
	const float *velocity = zero_vector;
	const float *view = zero_vector;
	uint8_t alive = 0;
	uint8_t on_ground = 0;
	uint8_t water_level = 0;
	uint8_t movement_mode = 2;

	if (!asset_id || !sensory_id || !output || dt_us == 0
			|| !nonzero_identity(asset_id) || !nonzero_identity(sensory_id))
	{
		return 0;
	}
	if (snapshot)
	{
		if (snapshot->alive > 1 || snapshot->on_ground > 1 || snapshot->water_level > 3
				|| snapshot->movement_mode > 2 || !canonical_vector(snapshot->origin)
				|| !canonical_vector(snapshot->velocity) || !canonical_view(snapshot->view))
		{
			return 0;
		}
		alive = snapshot->alive;
		on_ground = snapshot->on_ground;
		water_level = snapshot->water_level;
		movement_mode = snapshot->movement_mode;
		origin = snapshot->origin;
		velocity = snapshot->velocity;
		view = snapshot->view;
	}

	memset(output, 0, SOL_CORE_OBSERVATION_V1_SIZE);
	memcpy(output, "SLO1", 4);
	write_u64_le(output + 4, frame_seq);
	write_u32_le(output + 12, dt_us);
	memcpy(output + 16, asset_id, 32);
	memcpy(output + 48, sensory_id, 32);
	output[SOL_OBSERVATION_ALIVE_OFFSET] = alive;
	output[SOL_OBSERVATION_ALIVE_OFFSET + 1] = on_ground;
	output[SOL_OBSERVATION_ALIVE_OFFSET + 2] = water_level;
	output[SOL_OBSERVATION_ALIVE_OFFSET + 3] = movement_mode;
	write_vector(output + SOL_OBSERVATION_ORIGIN_OFFSET, origin);
	write_vector(output + SOL_OBSERVATION_VELOCITY_OFFSET, velocity);
	write_vector(output + SOL_OBSERVATION_VIEW_OFFSET, view);
	write_u32_le(output + SOL_OBSERVATION_SIGHT_COUNT_OFFSET, 0);
	return 1;
}

int sol_ktx_decode_action_v1(const uint8_t *action, size_t action_size,
		uint64_t expected_frame_seq, uint8_t msec, sol_ktx_command_v1 *output)
{
	float view[3];

	if (!action || !output || action_size != SOL_CORE_ACTION_V1_SIZE || msec == 0
			|| memcmp(action, "SLA1", 4) || read_u64_le(action + 4) != expected_frame_seq
			|| !read_f32(action + SOL_ACTION_VIEW_OFFSET, &view[0])
			|| !read_f32(action + SOL_ACTION_VIEW_OFFSET + 4, &view[1])
			|| !read_f32(action + SOL_ACTION_VIEW_OFFSET + 8, &view[2])
			|| !canonical_view(view) || action[SOL_ACTION_TEAMSAY_OFFSET] != 0)
	{
		return 0;
	}
	memset(output, 0, sizeof(*output));
	output->msec = msec;
	memcpy(output->angles, view, sizeof(view));
	output->forwardmove = (int16_t) read_u16_le(action + SOL_ACTION_FORWARD_OFFSET);
	output->sidemove = (int16_t) read_u16_le(action + SOL_ACTION_FORWARD_OFFSET + 2);
	output->upmove = (int16_t) read_u16_le(action + SOL_ACTION_FORWARD_OFFSET + 4);
	output->buttons = action[SOL_ACTION_BUTTONS_OFFSET];
	output->impulse = action[SOL_ACTION_WEAPON_OFFSET];
	return 1;
}

int sol_ktx_encode_command_v1(const sol_ktx_command_v1 *command,
		uint8_t output[SOL_KTX_COMMAND_V1_SIZE])
{
	if (!command || !output || command->msec == 0 || !canonical_vector(command->angles))
	{
		return 0;
	}
	memcpy(output, "SUC1", 4);
	output[4] = command->msec;
	write_vector(output + 5, command->angles);
	write_u16_le(output + 17, (uint16_t) command->forwardmove);
	write_u16_le(output + 19, (uint16_t) command->sidemove);
	write_u16_le(output + 21, (uint16_t) command->upmove);
	output[23] = command->buttons;
	output[24] = command->impulse;
	return 1;
}

const sol_ktx_seat_identity_v1 *sol_ktx_plan_identity_v1(const char *plan_seat)
{
	static const sol_ktx_seat_identity_v1 identities[SOL_KTX_CANDIDATE_COUNT_V1] = {
		{ 1u, "1", "candidate-1", "cand-1" },
		{ 2u, "2", "candidate-2", "cand-2" },
		{ 3u, "3", "candidate-3", "cand-3" },
		{ 4u, "4", "candidate-4", "cand-4" }
	};
	unsigned index;

	if (!plan_seat)
	{
		return NULL;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		if (!strcmp(plan_seat, identities[index].plan_seat))
		{
			return &identities[index];
		}
	}
	return NULL;
}

int sol_ktx_plan_seat_v1(const char *plan_seat, char *evidence_seat, size_t capacity)
{
	const sol_ktx_seat_identity_v1 *identity = sol_ktx_plan_identity_v1(plan_seat);
	size_t length;

	if (!identity || !evidence_seat)
	{
		return 0;
	}
	length = strlen(identity->evidence_seat) + 1u;
	if (capacity < length)
	{
		return 0;
	}
	memcpy(evidence_seat, identity->evidence_seat, length);
	return 1;
}

int sol_ktx_add_shape_v1(const char *skill_token, const char *team)
{
	return skill_token && team && !strcmp(skill_token, "20") && !strcmp(team, "red");
}
