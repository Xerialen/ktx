#include "sol_core.h"

#include <stdlib.h>
#include <string.h>

#define SOL_INIT_ASSET_ID_OFFSET 4
#define SOL_INIT_SENSORY_ID_OFFSET 36
#define SOL_INIT_GOAL_ID_OFFSET 68
#define SOL_INIT_GOAL_OFFSET 100
#define SOL_INIT_RADIUS_OFFSET 112

#define SOL_OBSERVATION_FRAME_OFFSET 4
#define SOL_OBSERVATION_DT_OFFSET 12
#define SOL_OBSERVATION_ASSET_ID_OFFSET 16
#define SOL_OBSERVATION_SENSORY_ID_OFFSET 48
#define SOL_OBSERVATION_ALIVE_OFFSET 80
#define SOL_OBSERVATION_ON_GROUND_OFFSET 81
#define SOL_OBSERVATION_WATER_LEVEL_OFFSET 82
#define SOL_OBSERVATION_MOVEMENT_MODE_OFFSET 83
#define SOL_OBSERVATION_ORIGIN_OFFSET 84
#define SOL_OBSERVATION_VELOCITY_OFFSET 96
#define SOL_OBSERVATION_VIEW_OFFSET 108
#define SOL_OBSERVATION_SIGHT_COUNT_OFFSET 120

#define SOL_ACTION_FRAME_OFFSET 4
#define SOL_ACTION_VIEW_OFFSET 12
#define SOL_ACTION_FORWARD_OFFSET 24
#define SOL_ACTION_SIDE_OFFSET 26
#define SOL_ACTION_UP_OFFSET 28
#define SOL_ACTION_BUTTONS_OFFSET 30
#define SOL_ACTION_WEAPON_OFFSET 31
#define SOL_ACTION_TEAMSAY_OFFSET 32

#define SOL_MOVE_SPEED 400
#define SOL_EXACT_LIMBS 9
#define SOL_SQUARED_LIMBS (SOL_EXACT_LIMBS * 2)

typedef struct sol_exact_integer
{
	uint32_t limb[SOL_EXACT_LIMBS];
	int negative;
} sol_exact_integer;

struct sol_core_v1
{
	uint8_t asset_id[32];
	uint8_t sensory_id[32];
	uint8_t goal_id[32];
	float goal[3];
	float goal_radius;
	uint64_t next_frame_seq;
};

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

static int canonical_float_bits(uint32_t bits)
{
	return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000)
			&& bits != UINT32_C(0x80000000);
}

static int read_canonical_f32(const uint8_t *bytes, float *value)
{
	uint32_t bits = read_u32_le(bytes);

	if (!canonical_float_bits(bits))
	{
		return 0;
	}
	memcpy(value, &bits, sizeof(*value));
	return 1;
}

static void exact_from_f32(float value, sol_exact_integer *exact)
{
	uint32_t bits;
	uint32_t exponent;
	uint32_t significand;
	uint32_t shift;
	size_t word;
	uint32_t bit;
	uint64_t shifted;

	memset(exact, 0, sizeof(*exact));
	memcpy(&bits, &value, sizeof(bits));
	exponent = (bits >> 23) & UINT32_C(0xff);
	if (exponent == 0)
	{
		significand = bits & UINT32_C(0x7fffff);
		shift = 0;
	}
	else
	{
		significand = UINT32_C(0x800000) | (bits & UINT32_C(0x7fffff));
		shift = exponent - 1;
	}
	exact->negative = (bits >> 31) != 0 && significand != 0;
	word = shift / 32;
	bit = shift % 32;
	shifted = (uint64_t) significand << bit;
	exact->limb[word] = (uint32_t) shifted;
	if (word + 1 < SOL_EXACT_LIMBS)
	{
		exact->limb[word + 1] = (uint32_t) (shifted >> 32);
	}
}

static int compare_magnitude(const sol_exact_integer *left, const sol_exact_integer *right)
{
	size_t index = SOL_EXACT_LIMBS;

	while (index-- > 0)
	{
		if (left->limb[index] != right->limb[index])
		{
			return left->limb[index] > right->limb[index] ? 1 : -1;
		}
	}
	return 0;
}

static void add_magnitude(sol_exact_integer *result, const sol_exact_integer *left,
		const sol_exact_integer *right)
{
	size_t index;
	uint64_t carry = 0;

	memset(result, 0, sizeof(*result));
	for (index = 0; index < SOL_EXACT_LIMBS; ++index)
	{
		uint64_t sum = (uint64_t) left->limb[index] + right->limb[index] + carry;

		result->limb[index] = (uint32_t) sum;
		carry = sum >> 32;
	}
}

static void subtract_magnitude(sol_exact_integer *result, const sol_exact_integer *larger,
		const sol_exact_integer *smaller)
{
	size_t index;
	uint64_t borrow = 0;

	memset(result, 0, sizeof(*result));
	for (index = 0; index < SOL_EXACT_LIMBS; ++index)
	{
		uint64_t left = larger->limb[index];
		uint64_t right = (uint64_t) smaller->limb[index] + borrow;

		result->limb[index] = (uint32_t) (left - right);
		borrow = left < right;
	}
}

static void exact_difference(float left_value, float right_value, sol_exact_integer *result)
{
	sol_exact_integer left;
	sol_exact_integer right;
	int comparison;

	exact_from_f32(left_value, &left);
	exact_from_f32(right_value, &right);
	if (left.negative != right.negative)
	{
		add_magnitude(result, &left, &right);
		result->negative = left.negative;
		return;
	}

	comparison = compare_magnitude(&left, &right);
	if (comparison == 0)
	{
		memset(result, 0, sizeof(*result));
	}
	else if (comparison > 0)
	{
		subtract_magnitude(result, &left, &right);
		result->negative = left.negative;
	}
	else
	{
		subtract_magnitude(result, &right, &left);
		result->negative = !left.negative;
	}
}

static void square_magnitude(const sol_exact_integer *value,
		uint32_t result[SOL_SQUARED_LIMBS])
{
	size_t left_index;

	memset(result, 0, SOL_SQUARED_LIMBS * sizeof(*result));
	for (left_index = 0; left_index < SOL_EXACT_LIMBS; ++left_index)
	{
		size_t right_index;
		uint64_t carry = 0;

		for (right_index = 0; right_index < SOL_EXACT_LIMBS; ++right_index)
		{
			size_t index = left_index + right_index;
			uint64_t product = (uint64_t) value->limb[left_index]
					* value->limb[right_index];
			uint64_t sum = (uint64_t) result[index] + product + carry;

			result[index] = (uint32_t) sum;
			carry = sum >> 32;
		}
		right_index = left_index + SOL_EXACT_LIMBS;
		while (carry && right_index < SOL_SQUARED_LIMBS)
		{
			uint64_t sum = (uint64_t) result[right_index] + carry;

			result[right_index] = (uint32_t) sum;
			carry = sum >> 32;
			right_index++;
		}
	}
}

static int exact_horizontal_outside(const sol_exact_integer *dx, const sol_exact_integer *dy,
		float radius_value)
{
	sol_exact_integer radius;
	uint32_t dx_squared[SOL_SQUARED_LIMBS];
	uint32_t dy_squared[SOL_SQUARED_LIMBS];
	uint32_t radius_squared[SOL_SQUARED_LIMBS];
	uint32_t distance_squared[SOL_SQUARED_LIMBS];
	uint64_t carry = 0;
	size_t index;

	exact_from_f32(radius_value, &radius);
	square_magnitude(dx, dx_squared);
	square_magnitude(dy, dy_squared);
	square_magnitude(&radius, radius_squared);
	for (index = 0; index < SOL_SQUARED_LIMBS; ++index)
	{
		uint64_t sum = (uint64_t) dx_squared[index] + dy_squared[index] + carry;

		distance_squared[index] = (uint32_t) sum;
		carry = sum >> 32;
	}
	for (index = SOL_SQUARED_LIMBS; index-- > 0;)
	{
		if (distance_squared[index] != radius_squared[index])
		{
			return distance_squared[index] > radius_squared[index];
		}
	}
	return 0;
}

static int exact_direction(const sol_exact_integer *value)
{
	size_t index;

	for (index = 0; index < SOL_EXACT_LIMBS; ++index)
	{
		if (value->limb[index] != 0)
		{
			return value->negative ? -1 : 1;
		}
	}
	return 0;
}

static void write_canonical_f32(uint8_t *bytes, float value)
{
	uint32_t bits;

	if (value == 0.0f)
	{
		value = 0.0f;
	}
	memcpy(&bits, &value, sizeof(bits));
	write_u32_le(bytes, bits);
}

static int nonzero_identity(const uint8_t *identity)
{
	size_t index;
	uint8_t any = 0;

	for (index = 0; index < 32; ++index)
	{
		any |= identity[index];
	}
	return any != 0;
}

static int read_vector3(const uint8_t *bytes, float value[3])
{
	return read_canonical_f32(bytes, &value[0])
			&& read_canonical_f32(bytes + 4, &value[1])
			&& read_canonical_f32(bytes + 8, &value[2]);
}

static int canonical_view(const float view[3])
{
	return view[0] >= -90.0f && view[0] <= 90.0f
			&& view[1] >= -180.0f && view[1] < 180.0f
			&& view[2] >= -180.0f && view[2] < 180.0f;
}

sol_core_v1 *sol_core_create_v1(const uint8_t *init_wire, size_t init_len)
{
	sol_core_v1 *core;
	float goal[3];
	float radius;

	if (!init_wire || init_len != SOL_CORE_INIT_V1_SIZE
			|| memcmp(init_wire, "SLI1", 4)
			|| !nonzero_identity(init_wire + SOL_INIT_ASSET_ID_OFFSET)
			|| !nonzero_identity(init_wire + SOL_INIT_SENSORY_ID_OFFSET)
			|| !nonzero_identity(init_wire + SOL_INIT_GOAL_ID_OFFSET)
			|| !read_vector3(init_wire + SOL_INIT_GOAL_OFFSET, goal)
			|| !read_canonical_f32(init_wire + SOL_INIT_RADIUS_OFFSET, &radius)
			|| radius <= 0.0f)
	{
		return NULL;
	}

	core = calloc(1, sizeof(*core));
	if (!core)
	{
		return NULL;
	}
	memcpy(core->asset_id, init_wire + SOL_INIT_ASSET_ID_OFFSET, sizeof(core->asset_id));
	memcpy(core->sensory_id, init_wire + SOL_INIT_SENSORY_ID_OFFSET, sizeof(core->sensory_id));
	memcpy(core->goal_id, init_wire + SOL_INIT_GOAL_ID_OFFSET, sizeof(core->goal_id));
	memcpy(core->goal, goal, sizeof(core->goal));
	core->goal_radius = radius;
	return core;
}

static int valid_observation(const sol_core_v1 *core, const uint8_t *wire, size_t length,
								 uint64_t *frame_seq, float origin[3], float view[3], uint8_t *alive,
								 uint8_t *movement_mode)
{
	float velocity[3];

	if (!wire || length != SOL_CORE_OBSERVATION_V1_SIZE || memcmp(wire, "SLO1", 4)
			|| read_u32_le(wire + SOL_OBSERVATION_DT_OFFSET) == 0
			|| memcmp(wire + SOL_OBSERVATION_ASSET_ID_OFFSET, core->asset_id, sizeof(core->asset_id))
			|| memcmp(wire + SOL_OBSERVATION_SENSORY_ID_OFFSET, core->sensory_id,
					sizeof(core->sensory_id))
			|| wire[SOL_OBSERVATION_ALIVE_OFFSET] > 1
			|| wire[SOL_OBSERVATION_ON_GROUND_OFFSET] > 1
			|| wire[SOL_OBSERVATION_WATER_LEVEL_OFFSET] > 3
			|| wire[SOL_OBSERVATION_MOVEMENT_MODE_OFFSET] > 2
			|| read_u32_le(wire + SOL_OBSERVATION_SIGHT_COUNT_OFFSET) != 0
			|| !read_vector3(wire + SOL_OBSERVATION_ORIGIN_OFFSET, origin)
			|| !read_vector3(wire + SOL_OBSERVATION_VELOCITY_OFFSET, velocity)
			|| !read_vector3(wire + SOL_OBSERVATION_VIEW_OFFSET, view)
			|| !canonical_view(view))
	{
		return 0;
	}

	*frame_seq = read_u64_le(wire + SOL_OBSERVATION_FRAME_OFFSET);
	if (*frame_seq != core->next_frame_seq)
	{
		return 0;
	}
	*alive = wire[SOL_OBSERVATION_ALIVE_OFFSET];
	*movement_mode = wire[SOL_OBSERVATION_MOVEMENT_MODE_OFFSET];
	return 1;
}

static void write_action(uint8_t *wire, uint64_t frame_seq, const float view[3], int forwardmove)
{
	memcpy(wire, "SLA1", 4);
	write_u64_le(wire + SOL_ACTION_FRAME_OFFSET, frame_seq);
	write_canonical_f32(wire + SOL_ACTION_VIEW_OFFSET, view[0]);
	write_canonical_f32(wire + SOL_ACTION_VIEW_OFFSET + 4, view[1]);
	write_canonical_f32(wire + SOL_ACTION_VIEW_OFFSET + 8, view[2]);
	write_u16_le(wire + SOL_ACTION_FORWARD_OFFSET, (uint16_t) (int16_t) forwardmove);
	write_u16_le(wire + SOL_ACTION_SIDE_OFFSET, 0);
	write_u16_le(wire + SOL_ACTION_UP_OFFSET, 0);
	wire[SOL_ACTION_BUTTONS_OFFSET] = 0;
	wire[SOL_ACTION_WEAPON_OFFSET] = 0;
	wire[SOL_ACTION_TEAMSAY_OFFSET] = 0;
}

static float sign_quantized_goal_yaw(const sol_exact_integer *dx, const sol_exact_integer *dy)
{
	int x_direction = exact_direction(dx);
	int y_direction = exact_direction(dy);

	if (x_direction > 0)
	{
		return y_direction > 0 ? 45.0f : y_direction < 0 ? -45.0f : 0.0f;
	}
	if (x_direction < 0)
	{
		return y_direction > 0 ? 135.0f : y_direction < 0 ? -135.0f : -180.0f;
	}
	return y_direction > 0 ? 90.0f : y_direction < 0 ? -90.0f : 0.0f;
}

sol_core_status_v1 sol_core_step_v1(sol_core_v1 *core,
									 const uint8_t *observation_wire, size_t observation_len,
									 uint8_t *action_wire, size_t action_capacity,
									 size_t *action_len)
{
	uint64_t frame_seq;
	float origin[3];
	float prior_view[3];
	float requested_view[3];
	sol_exact_integer dx;
	sol_exact_integer dy;
	uint8_t alive;
	uint8_t movement_mode;
	int active;

	if (!action_len)
	{
		return SOL_CORE_BAD_ARGUMENT;
	}
	*action_len = 0;
	if (!core || !observation_wire || !action_wire)
	{
		return SOL_CORE_BAD_ARGUMENT;
	}
	if (action_capacity < SOL_CORE_ACTION_V1_SIZE)
	{
		return SOL_CORE_OUTPUT_TOO_SMALL;
	}
	if (!valid_observation(core, observation_wire, observation_len, &frame_seq, origin, prior_view,
			&alive, &movement_mode))
	{
		return SOL_CORE_BAD_OBSERVATION;
	}

	exact_difference(core->goal[0], origin[0], &dx);
	exact_difference(core->goal[1], origin[1], &dy);
	active = alive && movement_mode == 0
			&& exact_horizontal_outside(&dx, &dy, core->goal_radius);
	if (active)
	{
		requested_view[0] = 0.0f;
		requested_view[1] = sign_quantized_goal_yaw(&dx, &dy);
		requested_view[2] = 0.0f;
		write_action(action_wire, frame_seq, requested_view, SOL_MOVE_SPEED);
	}
	else
	{
		write_action(action_wire, frame_seq, prior_view, 0);
	}
	*action_len = SOL_CORE_ACTION_V1_SIZE;
	core->next_frame_seq++;
	return active ? SOL_CORE_OK : SOL_CORE_NEUTRAL;
}

void sol_core_destroy_v1(sol_core_v1 *core)
{
	free(core);
}
