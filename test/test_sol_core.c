#include "sol_core.h"

#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static const uint8_t init_goal_northeast[SOL_CORE_INIT_V1_SIZE] = {
	'S', 'L', 'I', '1',
	/* immutable static-asset identity */
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	/* sensory-profile identity */
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	/* immutable goal identity */
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	/* goal=(100,100,24), horizontal arrival radius=16 */
	0x00, 0x00, 0xc8, 0x42, 0x00, 0x00, 0xc8, 0x42,
	0x00, 0x00, 0xc0, 0x41, 0x00, 0x00, 0x80, 0x41
};

static const uint8_t observation_at_origin[SOL_CORE_OBSERVATION_V1_SIZE] = {
	'S', 'L', 'O', '1',
	/* frame_seq=0, dt_us=13000 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xc8, 0x32, 0x00, 0x00,
	/* identities must match init */
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	/* alive, on_ground, water_level=0, movement_mode=normal */
	0x01, 0x01, 0x00, 0x00,
	/* origin=(0,0,24), velocity=(0,0,0), view=(10,90,0) */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xc0, 0x41,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0xb4, 0x42,
	0x00, 0x00, 0x00, 0x00,
	/* zero retained sight values */
	0x00, 0x00, 0x00, 0x00
};

static const uint8_t expected_northeast_action[SOL_CORE_ACTION_V1_SIZE] = {
	'S', 'L', 'A', '1',
	/* frame_seq=0 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* view=(0,45,0), complete movement=(400,0,0) */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x42,
	0x00, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x00,
	0x00, 0x00,
	/* buttons=0, weapon=KEEP, no teamsay */
	0x00, 0x00, 0x00
};

static const uint8_t expected_arrived_action_frame_1[SOL_CORE_ACTION_V1_SIZE] = {
	'S', 'L', 'A', '1',
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* neutral preserves the last allowed self view=(10,90,0) */
	0x00, 0x00, 0x20, 0x41, 0x00, 0x00, 0xb4, 0x42,
	0x00, 0x00, 0x00, 0x00,
	/* complete zero movement/buttons, KEEP, no teamsay */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00
};

static const uint8_t expected_west_action[SOL_CORE_ACTION_V1_SIZE] = {
	'S', 'L', 'A', '1',
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* canonical yaw range is [-180,180), so west is -180 rather than +180 */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0xc3,
	0x00, 0x00, 0x00, 0x00,
	0x90, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00
};

static const uint8_t expected_east_action[SOL_CORE_ACTION_V1_SIZE] = {
	'S', 'L', 'A', '1',
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x90, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00
};

static const uint8_t expected_southwest_action[SOL_CORE_ACTION_V1_SIZE] = {
	'S', 'L', 'A', '1',
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* view=(0,-135,0), complete movement=(400,0,0) */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xc3,
	0x00, 0x00, 0x00, 0x00,
	0x90, 0x01, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00
};

static void expect(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_goal_directed_action_has_exact_canonical_bytes(void)
{
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len = 999;
	sol_core_v1 *core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_status_v1 status;

	expect(core != NULL, "valid immutable-goal init creates a core");
	if (!core)
	{
		return;
	}
	memset(action, 0xa5, sizeof(action));
	status = sol_core_step_v1(core, observation_at_origin, sizeof(observation_at_origin),
			action, sizeof(action), &action_len);
	expect(status == SOL_CORE_OK, "distant live self produces an active action");
	expect(action_len == sizeof(expected_northeast_action), "action has fixed v1 length");
	expect(!memcmp(action, expected_northeast_action, sizeof(action)),
			"goal-directed action matches the independent worked byte vector");
	sol_core_destroy_v1(core);
}

static void test_zero_elapsed_time_is_rejected_without_partial_output(void)
{
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t untouched[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len = 999;
	sol_core_v1 *core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_status_v1 status;

	expect(core != NULL, "zero-time test creates a core");
	if (!core)
	{
		return;
	}
	memcpy(observation, observation_at_origin, sizeof(observation));
	memset(observation + 12, 0, 4);
	memset(action, 0xa5, sizeof(action));
	memcpy(untouched, action, sizeof(untouched));
	status = sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
			&action_len);
	expect(status == SOL_CORE_BAD_OBSERVATION, "zero dt_us is not a bot frame");
	expect(action_len == 0, "failed observation publishes no action length");
	expect(!memcmp(action, untouched, sizeof(action)), "failed observation publishes no partial bytes");
	sol_core_destroy_v1(core);
}

static void test_arrival_emits_exact_fresh_neutral_action(void)
{
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len;
	sol_core_v1 *core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_status_v1 status;

	expect(core != NULL, "arrival test creates a core");
	if (!core)
	{
		return;
	}
	status = sol_core_step_v1(core, observation_at_origin, sizeof(observation_at_origin),
			action, sizeof(action), &action_len);
	expect(status == SOL_CORE_OK, "arrival test advances through frame zero");

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[4] = 1;
	/* origin=(96,100,24), four units from the immutable goal */
	memcpy(observation + 84, "\x00\x00\xc0\x42\x00\x00\xc8\x42", 8);
	memset(action, 0xa5, sizeof(action));
	action_len = 999;
	status = sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
			&action_len);
	expect(status == SOL_CORE_NEUTRAL, "arrival radius produces a deliberate neutral action");
	expect(action_len == sizeof(expected_arrived_action_frame_1), "neutral action is complete");
	expect(!memcmp(action, expected_arrived_action_frame_1, sizeof(action)),
			"arrival neutral matches the independent worked byte vector");
	sol_core_destroy_v1(core);
}

static void test_generated_west_yaw_is_canonical_negative_180(void)
{
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len;
	sol_core_v1 *core;
	sol_core_status_v1 status;

	memcpy(init, init_goal_northeast, sizeof(init));
	/* goal=(-100,0,24) */
	memcpy(init + 100,
			"\x00\x00\xc8\xc2\x00\x00\x00\x00\x00\x00\xc0\x41", 12);
	core = sol_core_create_v1(init, sizeof(init));
	expect(core != NULL, "west-goal init creates a core");
	if (!core)
	{
		return;
	}
	status = sol_core_step_v1(core, observation_at_origin, sizeof(observation_at_origin),
			action, sizeof(action), &action_len);
	expect(status == SOL_CORE_OK, "west goal produces an active action");
	expect(action_len == sizeof(expected_west_action), "west action is complete");
	expect(!memcmp(action, expected_west_action, sizeof(action)),
			"generated west yaw uses the canonical -180 representation");
	sol_core_destroy_v1(core);
}

static void test_finite_large_goal_does_not_overflow_into_false_arrival(void)
{
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len;
	sol_core_v1 *core;
	sol_core_status_v1 status;

	memcpy(init, init_goal_northeast, sizeof(init));
	/* goal=(1e20,0,24): finite f32, but its squared distance overflows f32. */
	memcpy(init + 100,
			"\xec\x78\xad\x60\x00\x00\x00\x00\x00\x00\xc0\x41", 12);
	core = sol_core_create_v1(init, sizeof(init));
	expect(core != NULL, "finite large-goal init creates a core");
	if (!core)
	{
		return;
	}
	status = sol_core_step_v1(core, observation_at_origin, sizeof(observation_at_origin),
			action, sizeof(action), &action_len);
	expect(status == SOL_CORE_OK, "finite distant goal cannot look like an arrival after overflow");
	expect(action_len == sizeof(expected_east_action), "large-goal action is complete");
	expect(!memcmp(action, expected_east_action, sizeof(action)),
			"large finite goal still produces the canonical east action");
	sol_core_destroy_v1(core);
}

static void test_action_bytes_ignore_ambient_rounding_mode(void)
{
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t nearest_action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t upward_action[SOL_CORE_ACTION_V1_SIZE];
	size_t nearest_len = 0;
	size_t upward_len = 0;
	int original_round = fegetround();
	sol_core_v1 *nearest_core;
	sol_core_v1 *upward_core;
	sol_core_status_v1 nearest_status;
	sol_core_status_v1 upward_status;

	memcpy(init, init_goal_northeast, sizeof(init));
	/* goal=(-1000,-1000,24), the exact independent southwest heading. */
	memcpy(init + 100,
			"\x00\x00\x7a\xc4\x00\x00\x7a\xc4\x00\x00\xc0\x41", 12);
	nearest_core = sol_core_create_v1(init, sizeof(init));
	upward_core = sol_core_create_v1(init, sizeof(init));
	expect(original_round != -1, "rounding-mode replay can inspect the ambient mode");
	expect(nearest_core != NULL && upward_core != NULL,
			"rounding-mode replay creates isolated cores");
	if (original_round == -1 || !nearest_core || !upward_core)
	{
		sol_core_destroy_v1(nearest_core);
		sol_core_destroy_v1(upward_core);
		return;
	}

	expect(fesetround(FE_TONEAREST) == 0, "rounding-mode replay selects nearest");
	nearest_status = sol_core_step_v1(nearest_core, observation_at_origin,
			sizeof(observation_at_origin), nearest_action, sizeof(nearest_action), &nearest_len);
	expect(fesetround(FE_UPWARD) == 0, "rounding-mode replay selects upward");
	upward_status = sol_core_step_v1(upward_core, observation_at_origin,
			sizeof(observation_at_origin), upward_action, sizeof(upward_action), &upward_len);
	expect(fesetround(original_round) == 0, "rounding-mode replay restores the ambient mode");

	expect(nearest_status == SOL_CORE_OK && upward_status == SOL_CORE_OK,
			"both ambient rounding modes produce an active action");
	expect(nearest_len == sizeof(expected_southwest_action)
				&& upward_len == sizeof(expected_southwest_action),
			"both ambient rounding modes produce complete actions");
	expect(!memcmp(nearest_action, expected_southwest_action, sizeof(nearest_action)),
			"nearest mode produces the pinned southwest bytes");
	expect(!memcmp(upward_action, expected_southwest_action, sizeof(upward_action)),
			"upward mode produces the same pinned southwest bytes");
	expect(!memcmp(nearest_action, upward_action, sizeof(nearest_action)),
			"identical wires replay identically across ambient rounding modes");
	sol_core_destroy_v1(nearest_core);
	sol_core_destroy_v1(upward_core);
}

static void test_arrival_classification_ignores_ambient_rounding_mode(void)
{
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t nearest_action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t upward_action[SOL_CORE_ACTION_V1_SIZE];
	size_t nearest_len = 0;
	size_t upward_len = 0;
	int original_round = fegetround();
	sol_core_v1 *nearest_core;
	sol_core_v1 *upward_core;
	sol_core_status_v1 nearest_status;
	sol_core_status_v1 upward_status;

	memcpy(init, init_goal_northeast, sizeof(init));
	/* goal=(2^100,0,24), radius=2^100. */
	memcpy(init + 100,
			"\x00\x00\x80\x71\x00\x00\x00\x00\x00\x00\xc0\x41", 12);
	memcpy(init + 112, "\x00\x00\x80\x71", 4);
	memcpy(observation, observation_at_origin, sizeof(observation));
	/* origin.x=-minimum-subnormal, so exact horizontal distance is just outside. */
	memcpy(observation + 84, "\x01\x00\x00\x80", 4);
	nearest_core = sol_core_create_v1(init, sizeof(init));
	upward_core = sol_core_create_v1(init, sizeof(init));
	expect(original_round != -1, "arrival replay can inspect the ambient mode");
	expect(nearest_core != NULL && upward_core != NULL,
			"arrival replay creates isolated cores");
	if (original_round == -1 || !nearest_core || !upward_core)
	{
		sol_core_destroy_v1(nearest_core);
		sol_core_destroy_v1(upward_core);
		return;
	}

	expect(fesetround(FE_TONEAREST) == 0, "arrival replay selects nearest");
	nearest_status = sol_core_step_v1(nearest_core, observation, sizeof(observation),
			nearest_action, sizeof(nearest_action), &nearest_len);
	expect(fesetround(FE_UPWARD) == 0, "arrival replay selects upward");
	upward_status = sol_core_step_v1(upward_core, observation, sizeof(observation),
			upward_action, sizeof(upward_action), &upward_len);
	expect(fesetround(original_round) == 0, "arrival replay restores the ambient mode");

	expect(nearest_status == SOL_CORE_OK && upward_status == SOL_CORE_OK,
			"exactly-outside distance stays active in both rounding modes");
	expect(nearest_len == sizeof(expected_east_action)
				&& upward_len == sizeof(expected_east_action),
			"arrival replay produces complete actions");
	expect(!memcmp(nearest_action, expected_east_action, sizeof(nearest_action))
				&& !memcmp(upward_action, expected_east_action, sizeof(upward_action)),
			"arrival replay produces the pinned exact east bytes");
	expect(!memcmp(nearest_action, upward_action, sizeof(nearest_action)),
			"arrival classification and action bytes ignore ambient rounding mode");
	sol_core_destroy_v1(nearest_core);
	sol_core_destroy_v1(upward_core);
}

static void test_dead_and_locked_self_emit_complete_neutral_actions(void)
{
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t expected[SOL_CORE_ACTION_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len;
	sol_core_v1 *core;
	sol_core_status_v1 status;

	memcpy(expected, expected_arrived_action_frame_1, sizeof(expected));
	expected[4] = 0;

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[80] = 0;
	observation[83] = 1;
	core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	expect(core != NULL, "dead-self test creates a core");
	if (core)
	{
		status = sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
				&action_len);
		expect(status == SOL_CORE_NEUTRAL, "dead self requests a neutral action");
		expect(action_len == sizeof(expected) && !memcmp(action, expected, sizeof(expected)),
				"dead neutral is fresh, complete, and preserves allowed view");
		sol_core_destroy_v1(core);
	}

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[83] = 2;
	core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	expect(core != NULL, "locked-self test creates a core");
	if (core)
	{
		status = sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
				&action_len);
		expect(status == SOL_CORE_NEUTRAL, "locked self requests a neutral action");
		expect(action_len == sizeof(expected) && !memcmp(action, expected, sizeof(expected)),
				"locked neutral is fresh and complete");
		sol_core_destroy_v1(core);
	}
}

static void expect_bad_observation(const char *name, const uint8_t *observation)
{
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t untouched[SOL_CORE_ACTION_V1_SIZE];
	size_t action_len = 999;
	sol_core_v1 *core = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_status_v1 status;

	expect(core != NULL, "bad-observation case creates a core");
	if (!core)
	{
		return;
	}
	memset(action, 0xa5, sizeof(action));
	memcpy(untouched, action, sizeof(untouched));
	status = sol_core_step_v1(core, observation, SOL_CORE_OBSERVATION_V1_SIZE, action,
			sizeof(action), &action_len);
	expect(status == SOL_CORE_BAD_OBSERVATION, name);
	expect(action_len == 0, "bad observation publishes no action length");
	expect(!memcmp(action, untouched, sizeof(action)), "bad observation publishes no partial bytes");
	/* A rejected frame cannot consume frame_seq=0. */
	status = sol_core_step_v1(core, observation_at_origin, sizeof(observation_at_origin), action,
			sizeof(action), &action_len);
	expect(status == SOL_CORE_OK, "rejected observation does not advance private frame state");
	sol_core_destroy_v1(core);
}

static void test_noncanonical_or_nonempty_observations_fail_closed(void)
{
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	static const struct
	{
		size_t offset;
		uint8_t value;
		const char *message;
	} closed_scalar_cases[] = {
		{ 80, 2, "alive outside 0..1 is noncanonical" },
		{ 81, 2, "on_ground outside 0..1 is noncanonical" },
		{ 82, 4, "water_level outside 0..3 is noncanonical" },
		{ 83, 3, "unknown movement mode is noncanonical" }
	};
	size_t index;

	memcpy(observation, observation_at_origin, sizeof(observation));
	memcpy(observation + 84, "\x00\x00\x00\x80", 4);
	expect_bad_observation("negative-zero self origin is noncanonical", observation);

	memcpy(observation, observation_at_origin, sizeof(observation));
	memcpy(observation + 96, "\x00\x00\xc0\x7f", 4);
	expect_bad_observation("NaN self velocity is noncanonical", observation);

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[48] ^= 0xff;
	expect_bad_observation("sensory identity mismatch fails closed", observation);

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[16] ^= 0xff;
	expect_bad_observation("asset identity mismatch fails closed", observation);

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[120] = 1;
	expect_bad_observation("disposable v1 accepts only certified zero sight", observation);

	memcpy(observation, observation_at_origin, sizeof(observation));
	memcpy(observation + 112, "\x00\x00\x34\x43", 4);
	expect_bad_observation("positive 180-degree input yaw is noncanonical", observation);

	for (index = 0; index < sizeof(closed_scalar_cases) / sizeof(closed_scalar_cases[0]); ++index)
	{
		memcpy(observation, observation_at_origin, sizeof(observation));
		observation[closed_scalar_cases[index].offset] = closed_scalar_cases[index].value;
		expect_bad_observation(closed_scalar_cases[index].message, observation);
	}

	memcpy(observation, observation_at_origin, sizeof(observation));
	observation[0] = 'X';
	expect_bad_observation("wrong observation magic is rejected", observation);
}

static void test_invalid_init_bytes_never_create_a_core(void)
{
	uint8_t init[SOL_CORE_INIT_V1_SIZE];

	memcpy(init, init_goal_northeast, sizeof(init));
	init[0] = 'X';
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "wrong init magic is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memset(init + 68, 0, 32);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "zero immutable goal identity is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memset(init + 4, 0, 32);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "zero asset identity is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memset(init + 36, 0, 32);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "zero sensory identity is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memcpy(init + 100, "\x00\x00\xc0\x7f", 4);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "NaN goal coordinate is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memcpy(init + 112, "\x00\x00\x00\x80", 4);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "negative-zero radius is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memset(init + 112, 0, 4);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "zero radius is rejected");

	memcpy(init, init_goal_northeast, sizeof(init));
	memcpy(init + 112, "\x00\x00\x80\xbf", 4);
	expect(sol_core_create_v1(init, sizeof(init)) == NULL, "negative radius is rejected");

	expect(sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast) - 1) == NULL,
			"truncated init is rejected");
	expect(sol_core_create_v1(NULL, 0) == NULL, "null init is rejected");
}

static void test_failure_and_replay_are_state_safe(void)
{
	uint8_t action_a[SOL_CORE_ACTION_V1_SIZE];
	uint8_t action_b[SOL_CORE_ACTION_V1_SIZE];
	uint8_t untouched[SOL_CORE_ACTION_V1_SIZE];
	size_t length_a = 999;
	size_t length_b = 999;
	sol_core_v1 *core_a = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_v1 *core_b = sol_core_create_v1(init_goal_northeast, sizeof(init_goal_northeast));
	sol_core_status_v1 status;

	expect(core_a != NULL && core_b != NULL, "replay test creates isolated cores");
	if (!core_a || !core_b)
	{
		sol_core_destroy_v1(core_a);
		sol_core_destroy_v1(core_b);
		return;
	}
	memset(action_a, 0x5a, sizeof(action_a));
	memcpy(untouched, action_a, sizeof(untouched));
	status = sol_core_step_v1(core_a, observation_at_origin, sizeof(observation_at_origin),
			action_a, sizeof(action_a) - 1, &length_a);
	expect(status == SOL_CORE_OUTPUT_TOO_SMALL && length_a == 0,
			"short output buffer fails without an action");
	expect(!memcmp(action_a, untouched, sizeof(action_a)), "short output buffer is not partially written");

	expect(sol_core_step_v1(core_a, observation_at_origin, sizeof(observation_at_origin),
				action_a, sizeof(action_a), &length_a) == SOL_CORE_OK,
			"failed output attempt does not consume frame zero");
	expect(sol_core_step_v1(core_b, observation_at_origin, sizeof(observation_at_origin),
				action_b, sizeof(action_b), &length_b) == SOL_CORE_OK,
			"second isolated core accepts the same frame");
	expect(length_a == length_b && !memcmp(action_a, action_b, length_a),
			"same init and observation replay to identical requested bytes");

	length_a = 999;
	status = sol_core_step_v1(core_a, observation_at_origin, sizeof(observation_at_origin),
			action_a, sizeof(action_a), &length_a);
	expect(status == SOL_CORE_BAD_OBSERVATION && length_a == 0,
			"stale frame sequence fails without output");
	sol_core_destroy_v1(core_a);
	sol_core_destroy_v1(core_b);
}

int main(void)
{
	test_goal_directed_action_has_exact_canonical_bytes();
	test_zero_elapsed_time_is_rejected_without_partial_output();
	test_arrival_emits_exact_fresh_neutral_action();
	test_generated_west_yaw_is_canonical_negative_180();
	test_finite_large_goal_does_not_overflow_into_false_arrival();
	test_action_bytes_ignore_ambient_rounding_mode();
	test_arrival_classification_ignores_ambient_rounding_mode();
	test_dead_and_locked_self_emit_complete_neutral_actions();
	test_noncanonical_or_nonempty_observations_fail_closed();
	test_invalid_init_bytes_never_create_a_core();
	test_failure_and_replay_are_state_safe();
	if (!failures)
	{
		printf("sol_core: 11 contract tests passed\n");
	}
	return failures ? 1 : 0;
}
