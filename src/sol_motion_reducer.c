#include "sol_motion_reducer.h"

#include <limits.h>
#include <string.h>

static uint64_t saturating_add_u64(uint64_t left, uint64_t right)
{
	return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static uint32_t saturating_increment_u32(uint32_t value)
{
	return value == UINT32_MAX ? value : value + 1u;
}

static uint64_t progress_squared(const int16_t left[3],
	const int16_t right[3], const sol_motion_intent_v1 *intent)
{
	uint64_t total = 0u;
	unsigned component_count = intent->forwardmove != 0 ||
		intent->sidemove != 0 ? 2u : 3u;
	unsigned component;

	for (component = 0u; component < component_count; ++component)
	{
		int64_t delta = (int64_t) left[component] - right[component];

		total += (uint64_t) (delta * delta);
	}
	return total;
}

static int sample_is_valid(const sol_motion_sample_v1 *sample)
{
	return sample && sample->dt_us != 0u && sample->can_move <= 1u
		&& !(sample->intent.epoch == 0u
			&& (sample->intent.forwardmove != 0
				|| sample->intent.sidemove != 0
				|| sample->intent.upmove != 0))
		&& sample->intent.forwardmove != INT16_MIN
		&& sample->intent.sidemove != INT16_MIN;
}

static void reset_tracking(sol_motion_state_v1 *state)
{
	state->no_progress_us = 0u;
	state->intent_epoch = 0u;
	state->recovery_bucket = 0u;
	memset(state->anchor, 0, sizeof(state->anchor));
	state->prior_active = 0u;
	state->recovering = 0u;
}

static void begin_tracking(sol_motion_state_v1 *state,
	const sol_motion_sample_v1 *sample)
{
	state->no_progress_us = 0u;
	state->intent_epoch = sample->intent.epoch;
	state->recovery_bucket = 0u;
	memcpy(state->anchor, sample->origin, sizeof(state->anchor));
	state->prior_active = 1u;
	state->recovering = 0u;
}

static void recovery_command(const sol_motion_intent_v1 *intent,
	uint8_t attempt, sol_motion_result_v1 *result)
{
	switch (attempt)
	{
	case 0u:
		result->forwardmove = (int16_t) -intent->sidemove;
		result->sidemove = intent->forwardmove;
		result->upmove = intent->upmove;
		break;
	case 1u:
		result->forwardmove = (int16_t) -intent->forwardmove;
		result->sidemove = (int16_t) -intent->sidemove;
		result->upmove = intent->upmove;
		break;
	case 2u:
		result->forwardmove = intent->sidemove;
		result->sidemove = (int16_t) -intent->forwardmove;
		result->upmove = intent->upmove;
		break;
	default:
		result->forwardmove = intent->forwardmove;
		result->sidemove = intent->sidemove;
		result->upmove = intent->upmove < SOL_MOTION_ESCAPE_UPMOVE_V1 ?
			SOL_MOTION_ESCAPE_UPMOVE_V1 : intent->upmove;
		break;
	}
}

sol_motion_status_v1 sol_motion_init_v1(sol_motion_state_v1 *state,
	const sol_motion_config_v1 *config)
{
	if (!state)
	{
		return SOL_MOTION_BAD_ARGUMENT_V1;
	}
	memset(state, 0, sizeof(*state));
	if (!config)
	{
		return SOL_MOTION_BAD_ARGUMENT_V1;
	}
	if (config->struct_size != sizeof(*config) || !config->stuck_replan_ms)
	{
		return SOL_MOTION_BAD_CONFIG_V1;
	}
	state->threshold_us = (uint64_t) config->stuck_replan_ms * UINT64_C(1000);
	state->initialized = 1u;
	return SOL_MOTION_OK_V1;
}

sol_motion_status_v1 sol_motion_step_v1(sol_motion_state_v1 *state,
	const sol_motion_sample_v1 *sample, sol_motion_result_v1 *result)
{
	sol_motion_state_v1 next;
	int active;

	if (result)
	{
		memset(result, 0, sizeof(*result));
	}
	if (!state || !sample || !result)
	{
		return SOL_MOTION_BAD_ARGUMENT_V1;
	}
	if (!state->initialized || !state->threshold_us)
	{
		return SOL_MOTION_BAD_STATE_V1;
	}
	if (!sample_is_valid(sample))
	{
		return SOL_MOTION_BAD_SAMPLE_V1;
	}
	if (state->have_frame && (state->last_frame_seq == UINT64_MAX
		|| sample->frame_seq != state->last_frame_seq + 1u))
	{
		return SOL_MOTION_SEQUENCE_ERROR_V1;
	}

	next = *state;
	active = sample->can_move && sample->intent.epoch != 0u;
	if (!active)
	{
		reset_tracking(&next);
		result->phase = SOL_MOTION_SUSPENDED_V1;
	}
	else if (!next.prior_active || next.intent_epoch != sample->intent.epoch)
	{
		begin_tracking(&next, sample);
		result->phase = SOL_MOTION_TRACKING_V1;
	}
	else
	{
		uint64_t progress_threshold_squared =
			(uint64_t) SOL_MOTION_PROGRESS_UNITS_V1
			* SOL_MOTION_PROGRESS_UNITS_V1;

		if (progress_squared(sample->origin, next.anchor, &sample->intent)
			>= progress_threshold_squared)
		{
			result->event_flags |= SOL_MOTION_PROGRESS_V1;
			if (next.recovering)
			{
				result->event_flags |= SOL_MOTION_STUCK_CLEARED_V1;
			}
			begin_tracking(&next, sample);
			result->phase = SOL_MOTION_TRACKING_V1;
		}
		else
		{
			uint64_t bucket;

			next.no_progress_us = saturating_add_u64(next.no_progress_us,
				next.prior_dt_us);
			bucket = next.no_progress_us / next.threshold_us;
			if (bucket == 0u)
			{
				result->phase = SOL_MOTION_TRACKING_V1;
			}
			else
			{
				if (!next.recovering)
				{
					next.recovering = 1u;
					next.episode_index =
						saturating_increment_u32(next.episode_index);
					result->event_flags |= SOL_MOTION_STUCK_STARTED_V1;
				}
				if (bucket != next.recovery_bucket)
				{
					result->event_flags |= SOL_MOTION_REPLAN_STARTED_V1;
					next.recovery_bucket = bucket;
				}
				result->phase = SOL_MOTION_RECOVERING_V1;
			}
		}
	}

	result->no_progress_us = next.no_progress_us;
	result->episode_index = next.episode_index;
	if (result->phase == SOL_MOTION_RECOVERING_V1)
	{
		result->recovery_attempt = (uint8_t)
			((next.recovery_bucket - 1u) % 4u);
		recovery_command(&sample->intent, result->recovery_attempt, result);
	}
	else if (result->phase == SOL_MOTION_TRACKING_V1)
	{
		result->forwardmove = sample->intent.forwardmove;
		result->sidemove = sample->intent.sidemove;
		result->upmove = sample->intent.upmove;
	}

	next.last_frame_seq = sample->frame_seq;
	next.prior_dt_us = sample->dt_us;
	next.have_frame = 1u;
	*state = next;
	return SOL_MOTION_OK_V1;
}
