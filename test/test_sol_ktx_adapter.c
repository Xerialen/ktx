#include "sol_core.h"
#include "sol_ktx_adapter.h"

#include <stdio.h>
#include <string.h>

static int failures;

static const uint8_t asset_id[32] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};
static const uint8_t sensory_id[32] = {
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
};
static const uint8_t goal_id[32] = {
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
};

static void expect(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void test_first_frame_without_snapshot_is_exact_neutral(void)
{
	static const uint8_t expected_command[SOL_KTX_COMMAND_V1_SIZE] = {
		'S', 'U', 'C', '1', 13,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	const float goal[3] = { 705.0f, 146.0f, 56.0f };
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE];
	sol_ktx_command_v1 command;
	sol_core_v1 *core;
	sol_core_status_v1 status;
	size_t action_size = 0;

	expect(sol_ktx_encode_init_v1(asset_id, sensory_id, goal_id, goal, 32.0f, init),
			"adapter encodes the immutable init");
	core = sol_core_create_v1(init, sizeof(init));
	expect(core != NULL, "encoded init creates the core");
	if (!core)
	{
		return;
	}
	expect(sol_ktx_encode_observation_v1(0, 13000, asset_id, sensory_id, NULL, observation),
			"adapter encodes a no-snapshot observation");
	expect(!memcmp(observation + 120, "\0\0\0\0", 4),
			"no-snapshot observation has exactly zero sight values");
	status = sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
			&action_size);
	expect(status == SOL_CORE_NEUTRAL, "first no-snapshot frame is deliberately neutral");
	expect(action_size == sizeof(action), "neutral core action is complete");
	expect(sol_ktx_decode_action_v1(action, action_size, 0, 13, &command),
			"adapter decodes the complete neutral action");
	expect(sol_ktx_encode_command_v1(&command, command_wire),
			"adapter encodes the requested command");
	expect(!memcmp(command_wire, expected_command, sizeof(expected_command)),
			"first requested command matches the independent 25-byte SUC1 vector");
	sol_core_destroy_v1(core);
}

static void test_previous_self_snapshot_drives_exact_active_command(void)
{
	static const uint8_t expected_command[SOL_KTX_COMMAND_V1_SIZE] = {
		'S', 'U', 'C', '1', 13,
		/* pitch=0, yaw=45, roll=0 */
		0, 0, 0, 0, 0, 0, 0x34, 0x42, 0, 0, 0, 0,
		/* forward=400, side=0, up=0, buttons=0, impulse=0 */
		0x90, 0x01, 0, 0, 0, 0, 0, 0
	};
	const float goal[3] = { 705.0f, 146.0f, 56.0f };
	sol_ktx_snapshot_v1 snapshot = {
		1, 1, 0, 0,
		{ 0.0f, 0.0f, 24.0f },
		{ 0.0f, 0.0f, 0.0f },
		{ 10.0f, 90.0f, 0.0f }
	};
	uint8_t init[SOL_CORE_INIT_V1_SIZE];
	uint8_t observation[SOL_CORE_OBSERVATION_V1_SIZE];
	uint8_t action[SOL_CORE_ACTION_V1_SIZE];
	uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE];
	sol_ktx_command_v1 command;
	sol_core_v1 *core;
	size_t action_size = 0;

	expect(sol_ktx_encode_init_v1(asset_id, sensory_id, goal_id, goal, 32.0f, init),
			"active adapter encodes init");
	core = sol_core_create_v1(init, sizeof(init));
	expect(core != NULL, "active adapter creates core");
	if (!core)
	{
		return;
	}
	expect(sol_ktx_encode_observation_v1(0, 13000, asset_id, sensory_id, &snapshot,
			observation), "previous post-think self snapshot encodes");
	expect(!memcmp(observation + 120, "\0\0\0\0", 4),
			"active observation still has exactly zero sight values");
	expect(sol_core_step_v1(core, observation, sizeof(observation), action, sizeof(action),
			&action_size) == SOL_CORE_OK, "distant snapshot produces active core action");
	expect(sol_ktx_decode_action_v1(action, action_size, 0, 13, &command),
			"active core action decodes completely");
	expect(sol_ktx_encode_command_v1(&command, command_wire),
			"active requested command encodes");
	expect(!memcmp(command_wire, expected_command, sizeof(expected_command)),
			"active requested command matches independent 25-byte SUC1 vector");
	sol_core_destroy_v1(core);
}

static void test_plan_seat_evidence_identity_and_skill_token_stay_distinct(void)
{
	char evidence_seat[32];

	expect(sol_ktx_plan_seat_v1("1", evidence_seat, sizeof(evidence_seat)),
			"numeric plan seat 1 is accepted");
	expect(!strcmp(evidence_seat, "candidate-1"),
			"plan seat 1 maps to canonical evidence seat candidate-1");
	expect(!sol_ktx_plan_seat_v1("20", evidence_seat, sizeof(evidence_seat)),
			"legacy skill token 20 is not an evidencebind seat");
	expect(sol_ktx_add_shape_v1("20", "red"),
			"legacy addsol shape accepts skill token 20 and team red");
	expect(!sol_ktx_add_shape_v1("1", "red"),
			"numeric plan seat 1 is not the addsol skill token");
}

int main(void)
{
	test_first_frame_without_snapshot_is_exact_neutral();
	test_previous_self_snapshot_drives_exact_active_command();
	test_plan_seat_evidence_identity_and_skill_token_stay_distinct();
	if (!failures)
	{
		printf("sol_ktx_adapter: 3 contract tests passed\n");
	}
	return failures ? 1 : 0;
}
