#include "sol_motion_reducer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static sol_motion_state_v1 initialized(uint32_t threshold_ms)
{
	sol_motion_config_v1 config = {0};
	sol_motion_state_v1 state;

	memset(&state, 0xa5, sizeof(state));
	config.struct_size = sizeof(config);
	config.stuck_replan_ms = threshold_ms;
	require(sol_motion_init_v1(&state, &config) == SOL_MOTION_OK_V1,
		"valid motion configuration initializes one private reducer");
	return state;
}

static sol_motion_sample_v1 drive(uint64_t frame_seq, uint32_t dt_us,
	int16_t x, int16_t y, int16_t z)
{
	sol_motion_sample_v1 sample = {0};

	sample.frame_seq = frame_seq;
	sample.dt_us = dt_us;
	sample.origin[0] = x;
	sample.origin[1] = y;
	sample.origin[2] = z;
	sample.can_move = 1u;
	sample.intent.epoch = 1u;
	sample.intent.forwardmove = 400;
	return sample;
}

static sol_motion_result_v1 step(sol_motion_state_v1 *state,
	const sol_motion_sample_v1 *sample)
{
	sol_motion_result_v1 result;

	memset(&result, 0xa5, sizeof(result));
	require(sol_motion_step_v1(state, sample, &result) == SOL_MOTION_OK_V1,
		"valid contiguous motion sample reduces successfully");
	return result;
}

static void require_tracking(const sol_motion_result_v1 *result,
	uint64_t no_progress_us)
{
	require(result->phase == SOL_MOTION_TRACKING_V1 &&
		result->no_progress_us == no_progress_us &&
		result->forwardmove == 400 && result->sidemove == 0 &&
		result->upmove == 0,
		"tracking result preserves the declared nominal movement");
}

static void test_configuration_fails_closed(void)
{
	sol_motion_config_v1 config = {0};
	sol_motion_state_v1 state;
	sol_motion_state_v1 zero = {0};

	memset(&state, 0xa5, sizeof(state));
	require(sol_motion_init_v1(NULL, &config) == SOL_MOTION_BAD_ARGUMENT_V1,
		"null state is rejected");
	require(sol_motion_init_v1(&state, NULL) == SOL_MOTION_BAD_ARGUMENT_V1,
		"null configuration is rejected");
	require(!memcmp(&state, &zero, sizeof(state)),
		"failed initialization clears caller-owned state");
	config.struct_size = sizeof(config) - 1u;
	config.stuck_replan_ms = 500u;
	require(sol_motion_init_v1(&state, &config) == SOL_MOTION_BAD_CONFIG_V1,
		"wrong configuration size is rejected");
	config.struct_size = sizeof(config);
	config.stuck_replan_ms = 0u;
	require(sol_motion_init_v1(&state, &config) == SOL_MOTION_BAD_CONFIG_V1,
		"zero stuck threshold is rejected");
}

static void test_exact_prior_frame_time_crosses_threshold(void)
{
	sol_motion_state_v1 state = initialized(1000u);
	sol_motion_sample_v1 sample = drive(0u, 400000u, 0, 0, 0);
	sol_motion_result_v1 result = step(&state, &sample);

	require_tracking(&result, 0u);
	sample.frame_seq = 1u;
	sample.dt_us = 600000u;
	result = step(&state, &sample);
	require_tracking(&result, 400000u);
	sample.frame_seq = 2u;
	sample.dt_us = 1u;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_RECOVERING_V1 &&
		result.no_progress_us == 1000000u && result.episode_index == 1u &&
		result.recovery_attempt == 0u &&
		(result.event_flags & (SOL_MOTION_STUCK_STARTED_V1 |
			SOL_MOTION_REPLAN_STARTED_V1)) ==
			(SOL_MOTION_STUCK_STARTED_V1 | SOL_MOTION_REPLAN_STARTED_V1) &&
		result.forwardmove == 0 && result.sidemove == 400 &&
		result.upmove == 0,
		"threshold charges completed prior intervals and starts right-strafe escape");
}

static void test_observed_translation_resets_progress_clock(void)
{
	sol_motion_state_v1 state = initialized(500u);
	sol_motion_sample_v1 sample = drive(0u, 300000u, 0, 0, 0);
	sol_motion_result_v1 result = step(&state, &sample);

	sample.frame_seq = 1u;
	sample.dt_us = 300000u;
	sample.origin[0] = 7;
	result = step(&state, &sample);
	require_tracking(&result, 300000u);
	sample.frame_seq = 2u;
	sample.origin[0] = 8;
	result = step(&state, &sample);
	require_tracking(&result, 0u);
	require((result.event_flags & SOL_MOTION_PROGRESS_V1) != 0u,
		"eight observed translation units are canonical progress");

	state = initialized(500u);
	sample = drive(0u, 300000u, 0, 0, 0);
	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	sample.origin[2] = 8;
	result = step(&state, &sample);
	require_tracking(&result, 300000u);
	require((result.event_flags & SOL_MOTION_PROGRESS_V1) == 0u,
		"recovery-induced vertical motion cannot clear a horizontal obstruction");

	state = initialized(500u);
	sample = drive(0u, 300000u, 0, 0, 0);
	sample.intent.forwardmove = 0;
	sample.intent.upmove = 400;
	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	sample.origin[2] = 8;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_TRACKING_V1 &&
		result.no_progress_us == 0u &&
		(result.event_flags & SOL_MOTION_PROGRESS_V1) != 0u,
		"vertical translation remains progress for an explicitly vertical intent");
}

static void test_hold_and_immobility_never_become_stuck(void)
{
	sol_motion_state_v1 state = initialized(100u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;

	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	sample.intent.epoch = 0u;
	sample.intent.forwardmove = 0;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_SUSPENDED_V1 &&
		result.no_progress_us == 0u && result.episode_index == 0u &&
		result.forwardmove == 0 && result.sidemove == 0 && result.upmove == 0,
		"intentional hold disarms progress accounting");
	sample.frame_seq = 2u;
	sample.dt_us = 1000000u;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_SUSPENDED_V1 &&
		result.episode_index == 0u,
		"arbitrarily long hold cannot produce a false stuck episode");
	sample.frame_seq = 3u;
	sample.can_move = 0u;
	sample.intent.epoch = 1u;
	sample.intent.forwardmove = 400;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_SUSPENDED_V1 &&
		result.episode_index == 0u,
		"dead or engine-locked self cannot produce a stuck episode");
}

static void test_new_intent_epoch_starts_fresh(void)
{
	sol_motion_state_v1 state = initialized(500u);
	sol_motion_sample_v1 sample = drive(0u, 400000u, 0, 0, 0);
	sol_motion_result_v1 result;

	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	sample.dt_us = 400000u;
	result = step(&state, &sample);
	require_tracking(&result, 400000u);
	sample.frame_seq = 2u;
	sample.intent.epoch = 2u;
	result = step(&state, &sample);
	require_tracking(&result, 0u);
	require(result.event_flags == 0u && result.episode_index == 0u,
		"strategic intent change resets the old goal without a false episode");
}

static void test_recovery_repertoire_is_persistent_and_deterministic(void)
{
	sol_motion_state_v1 state = initialized(100u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;

	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 0u && result.forwardmove == 0 &&
		result.sidemove == 400 && result.upmove == 0,
		"recovery attempt zero rotates nominal motion right");
	sample.frame_seq = 2u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 1u && result.forwardmove == -400 &&
		result.sidemove == 0 && result.upmove == 0,
		"recovery attempt one reverses nominal motion");
	sample.frame_seq = 3u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 2u && result.forwardmove == 0 &&
		result.sidemove == -400 && result.upmove == 0,
		"recovery attempt two rotates nominal motion left");
	sample.frame_seq = 4u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 3u && result.forwardmove == 400 &&
		result.sidemove == 0 && result.upmove == 400,
		"recovery attempt three combines forward drive with one jump command");
	sample.frame_seq = 5u;
	sample.dt_us = 50000u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 0u && result.forwardmove == 0 &&
		result.sidemove == 400,
		"persistent obstruction cycles through the versioned four-action repertoire");
	sample.frame_seq = 6u;
	result = step(&state, &sample);
	require(result.recovery_attempt == 0u &&
		(result.event_flags & SOL_MOTION_REPLAN_STARTED_V1) == 0u &&
		result.forwardmove == 0 && result.sidemove == 400,
		"one recovery action persists between exact threshold boundaries");
}

static void test_progress_ends_one_recovery_episode(void)
{
	sol_motion_state_v1 state = initialized(100u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;

	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_RECOVERING_V1,
		"stationary drive enters recovery");
	sample.frame_seq = 2u;
	sample.origin[1] = 8;
	result = step(&state, &sample);
	require_tracking(&result, 0u);
	require((result.event_flags & (SOL_MOTION_PROGRESS_V1 |
			SOL_MOTION_STUCK_CLEARED_V1)) ==
			(SOL_MOTION_PROGRESS_V1 | SOL_MOTION_STUCK_CLEARED_V1) &&
		result.episode_index == 1u,
		"physical progress clears recovery but preserves monotonic episode identity");
}

static void test_recovery_jump_cannot_clear_a_wall_pinned_episode(void)
{
	sol_motion_state_v1 state = initialized(100u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;
	uint64_t frame;

	(void) step(&state, &sample);
	for (frame = 1u; frame <= 4u; ++frame)
	{
		sample.frame_seq = frame;
		result = step(&state, &sample);
	}
	require(result.phase == SOL_MOTION_RECOVERING_V1 &&
		result.recovery_attempt == 3u && result.upmove == 400,
		"pinned episode reaches its jump-assisted recovery attempt");
	sample.frame_seq = 5u;
	sample.origin[2] = 16;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_RECOVERING_V1 &&
		result.episode_index == 1u &&
		(result.event_flags & (SOL_MOTION_PROGRESS_V1 |
			SOL_MOTION_STUCK_CLEARED_V1)) == 0u,
		"vertical jump without horizontal displacement remains the same stuck episode");
}

static void test_velocity_without_translation_is_not_progress(void)
{
	sol_motion_state_v1 state = initialized(100u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;

	(void) step(&state, &sample);
	sample.frame_seq = 1u;
	sample.velocity[0] = 400;
	result = step(&state, &sample);
	require(result.phase == SOL_MOTION_RECOVERING_V1 &&
		(result.event_flags & SOL_MOTION_STUCK_STARTED_V1) != 0u,
		"reported velocity cannot conceal a physically pinned self position");
}

static void test_protocol_failure_is_transactional(void)
{
	sol_motion_state_v1 state = initialized(500u);
	sol_motion_sample_v1 sample = drive(0u, 100000u, 0, 0, 0);
	sol_motion_result_v1 result;
	sol_motion_state_v1 before;
	sol_motion_result_v1 zero = {0};

	(void) step(&state, &sample);
	before = state;
	sample.frame_seq = 0u;
	memset(&result, 0xa5, sizeof(result));
	require(sol_motion_step_v1(&state, &sample, &result) ==
		SOL_MOTION_SEQUENCE_ERROR_V1,
		"duplicate frame fails closed");
	require(!memcmp(&state, &before, sizeof(state)) &&
		!memcmp(&result, &zero, sizeof(result)),
		"failed reduction changes neither state nor zeroed output");
	sample.frame_seq = 2u;
	require(sol_motion_step_v1(&state, &sample, &result) ==
		SOL_MOTION_SEQUENCE_ERROR_V1,
		"frame gap also fails closed");
	sample.frame_seq = 1u;
	sample.dt_us = 0u;
	require(sol_motion_step_v1(&state, &sample, &result) ==
		SOL_MOTION_BAD_SAMPLE_V1,
		"zero observation duration fails closed");
}

static void test_replay_and_seats_are_byte_deterministic(void)
{
	sol_motion_state_v1 left = initialized(250u);
	sol_motion_state_v1 right = initialized(250u);
	sol_motion_sample_v1 sample;
	unsigned index;

	for (index = 0u; index < 12u; ++index)
	{
		sol_motion_result_v1 a;
		sol_motion_result_v1 b;

		sample = drive(index, index % 2u ? 70000u : 30000u,
			index == 9u ? 8 : 0, 0, 0);
		a = step(&left, &sample);
		b = step(&right, &sample);
		require(!memcmp(&a, &b, sizeof(a)) &&
			!memcmp(&left, &right, sizeof(left)),
			"identical observer streams produce byte-identical private state and facts");
	}
}

int main(void)
{
	test_configuration_fails_closed();
	test_exact_prior_frame_time_crosses_threshold();
	test_observed_translation_resets_progress_clock();
	test_hold_and_immobility_never_become_stuck();
	test_new_intent_epoch_starts_fresh();
	test_recovery_repertoire_is_persistent_and_deterministic();
	test_progress_ends_one_recovery_episode();
	test_recovery_jump_cannot_clear_a_wall_pinned_episode();
	test_velocity_without_translation_is_not_progress();
	test_protocol_failure_is_transactional();
	test_replay_and_seats_are_byte_deterministic();
	printf("sol_motion_reducer: 11 contract tests passed\n");
	return 0;
}
