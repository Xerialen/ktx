#include "sol_brain.h"

#include "sol_decision_trace.h"
#include "sol_motion_reducer.h"
#include "sol_wire.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SOL_OBSERVATION_FRAME_OFFSET 4u
#define SOL_OBSERVATION_DT_US_OFFSET 12u
#define SOL_OBSERVATION_EVENT_COUNT_OFFSET 98u
#define SOL_OBSERVATION_EVENTS_OFFSET 102u
#define SOL_OBSERVATION_ASSET_ID_OFFSET 16u
#define SOL_OBSERVATION_SENSORY_ID_OFFSET 48u

#define SOL_MOVEMENT_DEAD_V1 1u
#define SOL_MOVEMENT_NORMAL_V1 0u
#define SOL_BUTTON_ATTACK_V1 1u
#define SOL_MOVE_SPEED_V1 400
#define SOL_PLAYER_CENTER_OFFSET_COORD_V1 32
#define SOL_RADIANS_TO_DEGREES_V1 57.2957795130823208768f

#define SOL_CHANNEL_SELF_V1 0u
#define SOL_CHANNEL_SIGHT_V1 1u
#define SOL_CHANNEL_SOUND_V1 2u
#define SOL_CHANNEL_TEAMSAY_V1 3u
#define SOL_CHANNEL_KILLFEED_V1 4u

#define SOL_KIND_SELF_V1 UINT16_C(0x0001)
#define SOL_KIND_DAMAGE_V1 UINT16_C(0x0002)
#define SOL_KIND_SIGHT_V1 UINT16_C(0x0101)
#define SOL_KIND_SOUND_V1 UINT16_C(0x0201)
#define SOL_KIND_TEAMSAY_V1 UINT16_C(0x0301)
#define SOL_KIND_KILLFEED_V1 UINT16_C(0x0401)

#define SOL_RENDER_PLAYER_V1 0u

_Static_assert((int)SOL_BRAIN_TRACE_MAX_V1 ==
	(int)SOL_DECISION_TRACE_TARGET_SIZE_V1,
	"brain trace buffer covers canonical SDT1");

typedef struct sol_brain_cursor_v1 {
	const uint8_t *bytes;
	size_t length;
	size_t offset;
	int valid;
} sol_brain_cursor_v1;

typedef struct sol_brain_facts_v1 {
	uint8_t alive;
	uint8_t movement_mode;
	int16_t origin[3];
	int16_t velocity[3];
	int16_t view_height;
	float view[3];
	uint8_t top_color;
	uint8_t bottom_color;
	int has_self;
	int has_sight;
	int has_target;
	uint16_t target_token;
	int16_t target_origin[3];
	uint8_t target_top_color;
	uint8_t target_bottom_color;
	uint64_t target_distance_squared;
} sol_brain_facts_v1;

struct sol_brain_v1 {
	sol_brain_bootstrap_v1 bootstrap;
	uint64_t next_frame_seq;
	uint8_t sac1[SOL_WIRE_ACTION_MAX_V1];
	size_t sac1_length;
	uint8_t sdt1[SOL_BRAIN_TRACE_MAX_V1];
	size_t sdt1_length;
	sol_motion_state_v1 motion;
	int poisoned;
};

static uint16_t read_u16(const uint8_t *bytes)
{
	return (uint16_t) bytes[0] | ((uint16_t) bytes[1] << 8);
}

static uint32_t read_u32(const uint8_t *bytes);

static int cursor_take(sol_brain_cursor_v1 *cursor, size_t length)
{
	if (!cursor->valid || cursor->offset > cursor->length
		|| length > cursor->length - cursor->offset)
	{
		cursor->valid = 0;
		return 0;
	}
	return 1;
}

static uint8_t cursor_u8(sol_brain_cursor_v1 *cursor)
{
	if (!cursor_take(cursor, 1u))
	{
		return 0u;
	}
	return cursor->bytes[cursor->offset++];
}

static uint16_t cursor_u16(sol_brain_cursor_v1 *cursor)
{
	uint16_t value;

	if (!cursor_take(cursor, 2u))
	{
		return 0u;
	}
	value = read_u16(cursor->bytes + cursor->offset);
	cursor->offset += 2u;
	return value;
}

static uint32_t cursor_u32(sol_brain_cursor_v1 *cursor)
{
	uint32_t value;

	if (!cursor_take(cursor, 4u))
	{
		return 0u;
	}
	value = read_u32(cursor->bytes + cursor->offset);
	cursor->offset += 4u;
	return value;
}

static uint64_t cursor_u64(sol_brain_cursor_v1 *cursor)
{
	uint64_t low = cursor_u32(cursor);
	uint64_t high = cursor_u32(cursor);

	return low | (high << 32);
}

static int cursor_skip(sol_brain_cursor_v1 *cursor, size_t length)
{
	if (!cursor_take(cursor, length))
	{
		return 0;
	}
	cursor->offset += length;
	return 1;
}

static uint32_t read_u32(const uint8_t *bytes)
{
	return (uint32_t) bytes[0] | ((uint32_t) bytes[1] << 8)
		| ((uint32_t) bytes[2] << 16) | ((uint32_t) bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t *bytes)
{
	return (uint64_t) read_u32(bytes) | ((uint64_t) read_u32(bytes + 4u) << 32);
}

static int nonzero_identity(const uint8_t identity[32])
{
	uint8_t any = 0u;
	size_t index;

	for (index = 0; index < 32u; ++index)
	{
		any |= identity[index];
	}
	return any != 0u;
}

static float observation_angle(uint16_t encoded)
{
	float angle = (float) encoded * (360.0f / 65536.0f);

	return encoded >= UINT16_C(0x8000) ? angle - 360.0f : angle;
}

static int parse_self(sol_brain_cursor_v1 *cursor, sol_brain_facts_v1 *facts)
{
	unsigned index;

	facts->alive = cursor_u8(cursor);
	(void) cursor_u8(cursor);
	(void) cursor_u8(cursor);
	(void) cursor_u8(cursor);
	facts->movement_mode = cursor_u8(cursor);
	for (index = 0; index < 3u; ++index)
	{
		facts->origin[index] = (int16_t) cursor_u16(cursor);
	}
	for (index = 0; index < 3u; ++index)
	{
		facts->velocity[index] = (int16_t) cursor_u16(cursor);
	}
	for (index = 0; index < 3u; ++index)
	{
		facts->view[index] = observation_angle(cursor_u16(cursor));
	}
	facts->view_height = (int16_t) cursor_u16(cursor);
	for (index = 0; index < 9u; ++index)
	{
		(void) cursor_u32(cursor);
	}
	(void) cursor_u16(cursor);
	(void) cursor_u8(cursor);
	(void) cursor_u16(cursor);
	facts->top_color = cursor_u8(cursor);
	facts->bottom_color = cursor_u8(cursor);
	facts->has_self = cursor->valid;
	return cursor->valid;
}

static uint64_t distance_squared(const int16_t left[3], const int16_t right[3])
{
	uint64_t total = 0u;
	unsigned index;

	for (index = 0; index < 3u; ++index)
	{
		int64_t delta = (int64_t) left[index] - right[index];

		total += (uint64_t) (delta * delta);
	}
	return total;
}

static int parse_sight(sol_brain_cursor_v1 *cursor, sol_brain_facts_v1 *facts)
{
	uint32_t seen_count;
	uint32_t anchor_count;
	uint32_t index;

	(void) cursor_u8(cursor);
	(void) cursor_u8(cursor);
	seen_count = cursor_u32(cursor);
	for (index = 0; index < seen_count; ++index)
	{
		uint16_t token = cursor_u16(cursor);
		uint8_t kind = cursor_u8(cursor);
		int16_t origin[3];
		uint8_t top_color = 0u;
		uint8_t bottom_color = 0u;
		uint8_t dead = 0u;
		uint8_t gib = 0u;
		uint8_t movement_mode = SOL_MOVEMENT_DEAD_V1;
		uint8_t weapon_present;
		unsigned component;

		(void) cursor_u16(cursor);
		for (component = 0; component < 3u; ++component)
		{
			origin[component] = (int16_t) cursor_u16(cursor);
		}
		(void) cursor_u8(cursor);
		(void) cursor_u8(cursor);
		if (kind == SOL_RENDER_PLAYER_V1)
		{
			for (component = 0; component < 3u; ++component)
			{
				(void) cursor_u16(cursor);
			}
			(void) cursor_u16(cursor);
			top_color = cursor_u8(cursor);
			bottom_color = cursor_u8(cursor);
			dead = cursor_u8(cursor);
			gib = cursor_u8(cursor);
			(void) cursor_u8(cursor);
			movement_mode = cursor_u8(cursor);
			for (component = 0; component < 3u; ++component)
			{
				(void) cursor_u16(cursor);
			}
		}
		else
		{
			(void) cursor_u8(cursor);
			(void) cursor_u8(cursor);
			(void) cursor_u8(cursor);
		}
		weapon_present = cursor_u8(cursor);
		if (weapon_present)
		{
			(void) cursor_u16(cursor);
		}
		if (kind == SOL_RENDER_PLAYER_V1 && !dead && !gib
			&& movement_mode == SOL_MOVEMENT_NORMAL_V1
			&& bottom_color != facts->bottom_color)
		{
			uint64_t distance = distance_squared(origin, facts->origin);

			if (!facts->has_target || distance < facts->target_distance_squared
				|| (distance == facts->target_distance_squared
					&& token < facts->target_token))
			{
				facts->has_target = 1;
				facts->target_token = token;
				memcpy(facts->target_origin, origin, sizeof(origin));
				facts->target_top_color = top_color;
				facts->target_bottom_color = bottom_color;
				facts->target_distance_squared = distance;
			}
		}
	}
	anchor_count = cursor_u32(cursor);
	for (index = 0; index < anchor_count; ++index)
	{
		(void) cursor_u16(cursor);
		(void) cursor_u8(cursor);
	}
	facts->has_sight = cursor->valid;
	return cursor->valid;
}

static int parse_text(sol_brain_cursor_v1 *cursor)
{
	uint16_t length = cursor_u16(cursor);

	return cursor_skip(cursor, length);
}

static int parse_observation(const uint8_t *sob1, size_t length,
	sol_brain_facts_v1 *facts)
{
	sol_brain_cursor_v1 cursor = { sob1, length, SOL_OBSERVATION_EVENTS_OFFSET, 1 };
	uint32_t count = read_u32(sob1 + SOL_OBSERVATION_EVENT_COUNT_OFFSET);
	uint32_t index;

	memset(facts, 0, sizeof(*facts));
	for (index = 0; index < count && cursor.valid; ++index)
	{
		uint8_t channel;
		uint16_t kind;

		(void) cursor_u64(&cursor);
		channel = cursor_u8(&cursor);
		kind = cursor_u16(&cursor);
		if (channel == SOL_CHANNEL_SELF_V1 && kind == SOL_KIND_SELF_V1)
		{
			if (!parse_self(&cursor, facts))
			{
				return 0;
			}
		}
		else if (channel == SOL_CHANNEL_SELF_V1 && kind == SOL_KIND_DAMAGE_V1)
		{
			if (!cursor_skip(&cursor, 8u))
			{
				return 0;
			}
		}
		else if (channel == SOL_CHANNEL_SIGHT_V1 && kind == SOL_KIND_SIGHT_V1)
		{
			if (!parse_sight(&cursor, facts))
			{
				return 0;
			}
		}
		else if (channel == SOL_CHANNEL_SOUND_V1 && kind == SOL_KIND_SOUND_V1)
		{
			if (!cursor_skip(&cursor, 6u))
			{
				return 0;
			}
		}
		else if ((channel == SOL_CHANNEL_TEAMSAY_V1 && kind == SOL_KIND_TEAMSAY_V1)
			|| (channel == SOL_CHANNEL_KILLFEED_V1 && kind == SOL_KIND_KILLFEED_V1))
		{
			if (!parse_text(&cursor))
			{
				return 0;
			}
		}
		else
		{
			return 0;
		}
	}
	return cursor.valid && cursor.offset == length && facts->has_self
		&& facts->has_sight;
}

static void aim_at_target(const sol_brain_facts_v1 *facts, float view[3])
{
	float dx = (float) facts->target_origin[0] - facts->origin[0];
	float dy = (float) facts->target_origin[1] - facts->origin[1];
	float eye_z = (float) facts->origin[2] + facts->view_height;
	float target_z = (float) facts->target_origin[2]
		+ SOL_PLAYER_CENTER_OFFSET_COORD_V1;
	float horizontal = hypotf(dx, dy);

	view[0] = atan2f(eye_z - target_z, horizontal)
		* SOL_RADIANS_TO_DEGREES_V1;
	view[1] = atan2f(dy, dx) * SOL_RADIANS_TO_DEGREES_V1;
	if (view[1] >= 180.0f)
	{
		view[1] -= 360.0f;
	}
	view[2] = 0.0f;
}

static sol_brain_status_v1 poison(sol_brain_v1 *brain,
	sol_brain_status_v1 status)
{
	brain->sac1_length = 0u;
	brain->sdt1_length = 0u;
	brain->poisoned = 1;
	return status;
}

sol_brain_v1 *sol_brain_open_v1(const sol_brain_bootstrap_v1 *bootstrap)
{
	sol_brain_v1 *brain;
	sol_motion_config_v1 motion_config = {0};

	if (!bootstrap || bootstrap->struct_size != sizeof(*bootstrap)
		|| bootstrap->stuck_replan_ms < SOL_BRAIN_STUCK_REPLAN_MIN_MS_V1
		|| bootstrap->stuck_replan_ms > SOL_BRAIN_STUCK_REPLAN_MAX_MS_V1
		|| !nonzero_identity(bootstrap->static_asset_set_id)
		|| !nonzero_identity(bootstrap->sensory_profile_id))
	{
		return NULL;
	}
	brain = calloc(1u, sizeof(*brain));
	if (!brain)
	{
		return NULL;
	}
	brain->bootstrap = *bootstrap;
	motion_config.struct_size = sizeof(motion_config);
	motion_config.stuck_replan_ms = bootstrap->stuck_replan_ms;
	if (sol_motion_init_v1(&brain->motion, &motion_config) != SOL_MOTION_OK_V1)
	{
		memset(brain, 0, sizeof(*brain));
		free(brain);
		return NULL;
	}
	return brain;
}

sol_brain_status_v1 sol_brain_decide_v1(sol_brain_v1 *brain,
	const uint8_t *sob1, size_t sob1_length,
	sol_brain_decision_view_v1 *decision)
{
	if (decision)
	{
		decision->sac1 = NULL;
		decision->sac1_length = 0;
		decision->sdt1 = NULL;
		decision->sdt1_length = 0;
	}
	if (!brain)
	{
		return SOL_BRAIN_BAD_ARGUMENT;
	}
	if (brain->poisoned)
	{
		return SOL_BRAIN_POISONED;
	}
	if (!sob1 || !decision)
	{
		return poison(brain, SOL_BRAIN_BAD_ARGUMENT);
	}
	brain->sac1_length = 0u;
	brain->sdt1_length = 0u;
	if (!sol_wire_observation_is_canonical_v1(sob1, sob1_length))
	{
		return poison(brain, SOL_BRAIN_BAD_OBSERVATION);
	}
	if (memcmp(sob1 + SOL_OBSERVATION_ASSET_ID_OFFSET,
			brain->bootstrap.static_asset_set_id, 32u)
		|| memcmp(sob1 + SOL_OBSERVATION_SENSORY_ID_OFFSET,
			brain->bootstrap.sensory_profile_id, 32u))
	{
		return poison(brain, SOL_BRAIN_PROFILE_MISMATCH);
	}
	{
		uint64_t frame_seq = read_u64(sob1 + SOL_OBSERVATION_FRAME_OFFSET);
		sol_action_response_v1 action = {0};
		sol_decision_trace_decision_v1 trace_decision = {0};
		sol_brain_facts_v1 facts;
		sol_motion_sample_v1 motion_sample = {0};
		sol_motion_result_v1 motion_result;
		int respawn;
		int explore;
		int engage;

		if (frame_seq != brain->next_frame_seq)
		{
			return poison(brain, SOL_BRAIN_SEQUENCE_ERROR);
		}
		if (!parse_observation(sob1, sob1_length, &facts))
		{
			return poison(brain, SOL_BRAIN_BAD_OBSERVATION);
		}
		action.frame_seq = frame_seq;
		memcpy(action.view_angles, facts.view, sizeof(action.view_angles));
		respawn = !facts.alive || facts.movement_mode == SOL_MOVEMENT_DEAD_V1;
		explore = !respawn
			&& facts.movement_mode == SOL_MOVEMENT_NORMAL_V1;
		engage = explore && facts.has_target;
		motion_sample.frame_seq = frame_seq;
		motion_sample.dt_us = read_u32(sob1 + SOL_OBSERVATION_DT_US_OFFSET);
		memcpy(motion_sample.origin, facts.origin, sizeof(motion_sample.origin));
		memcpy(motion_sample.velocity, facts.velocity,
			sizeof(motion_sample.velocity));
		motion_sample.can_move = explore ? 1u : 0u;
		if (explore)
		{
			motion_sample.intent.epoch = 1u;
			motion_sample.intent.forwardmove = SOL_MOVE_SPEED_V1;
		}
		if (sol_motion_step_v1(&brain->motion, &motion_sample, &motion_result)
			!= SOL_MOTION_OK_V1)
		{
			return poison(brain, SOL_BRAIN_INTERNAL_ERROR);
		}
		if (respawn)
		{
			action.buttons = SOL_BUTTON_ATTACK_V1;
			trace_decision.decision_class = SOL_DECISION_CLASS_RESPAWN_V1;
			trace_decision.attack_gate = SOL_ATTACK_GATE_RESPAWN_ONLY_V1;
		}
		else if (explore)
		{
			action.forwardmove = motion_result.forwardmove;
			action.sidemove = motion_result.sidemove;
			action.upmove = motion_result.upmove;
			trace_decision.decision_class = SOL_DECISION_CLASS_EXPLORE_V1;
			if (engage)
			{
				aim_at_target(&facts, action.view_angles);
				action.buttons = SOL_BUTTON_ATTACK_V1;
				trace_decision.decision_class =
					SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1;
				trace_decision.attack_gate =
					SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1;
				trace_decision.selected_sighting_tag =
					SOL_SELECTED_SIGHTING_PRESENT_V1;
				trace_decision.selected_sighting_token = facts.target_token;
			}
		}
		if (!sol_wire_encode_action_v1(&action, brain->sac1,
				sizeof(brain->sac1), &brain->sac1_length))
		{
			return poison(brain, SOL_BRAIN_INTERNAL_ERROR);
		}
		if (!sol_decision_trace_encode_v1(sob1, sob1_length, brain->sac1,
			brain->sac1_length, &trace_decision, brain->sdt1,
			sizeof(brain->sdt1), &brain->sdt1_length))
		{
			return poison(brain, SOL_BRAIN_INTERNAL_ERROR);
		}
		brain->next_frame_seq++;
		decision->sac1 = brain->sac1;
		decision->sac1_length = brain->sac1_length;
		decision->sdt1 = brain->sdt1;
		decision->sdt1_length = brain->sdt1_length;
		return respawn || explore ? SOL_BRAIN_DECISION : SOL_BRAIN_NEUTRAL;
	}
}

void sol_brain_close_v1(sol_brain_v1 *brain)
{
	if (brain)
	{
		memset(brain, 0, sizeof(*brain));
	}
	free(brain);
}
