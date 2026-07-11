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

static void test_canonical_sac1_decodes_atomically(void)
{
	static const uint8_t action[33] = {
		'S', 'A', 'C', '1', 42, 0, 0, 0, 0, 0, 0, 0,
		/* pitch=-10, yaw=45, roll=0 */
		0, 0, 0x20, 0xc1, 0, 0, 0x34, 0x42, 0, 0, 0, 0,
		/* forward=400, side=-25, up=16, buttons=attack+jump, RL, no chat */
		0x90, 0x01, 0xe7, 0xff, 0x10, 0, 3, 7, 0
	};
	uint8_t mutated[37];
	sol_ktx_command_v1 command;
	sol_ktx_command_v1 before;

	memset(&command, 0xa5, sizeof(command));
	expect(sol_ktx_decode_sac1_v1(action, sizeof(action), 42, 13, &command),
			"canonical SAC1 decodes through the command-only adapter");
	expect(command.msec == 13 && command.angles[0] == -10.0f
			&& command.angles[1] == 45.0f && command.angles[2] == 0.0f
			&& command.forwardmove == 400 && command.sidemove == -25
			&& command.upmove == 16 && command.buttons == 3
			&& command.impulse == 7,
			"SAC1 fields project exactly into the KTX command");

	memset(&before, 0xa5, sizeof(before));
	command = before;
	expect(!sol_ktx_decode_sac1_v1(action, sizeof(action), 43, 13, &command)
			&& !memcmp(&command, &before, sizeof(command)),
			"wrong-frame SAC1 fails without partial output");
	command = before;
	expect(!sol_ktx_decode_sac1_v1(action, sizeof(action) - 1u, 42, 13, &command)
			&& !memcmp(&command, &before, sizeof(command)),
			"truncated SAC1 fails without partial output");
	memcpy(mutated, action, sizeof(action));
	mutated[sizeof(action)] = 0;
	command = before;
	expect(!sol_ktx_decode_sac1_v1(mutated, sizeof(action) + 1u, 42, 13, &command)
			&& !memcmp(&command, &before, sizeof(command)),
			"trailing SAC1 bytes fail without partial output");
	memcpy(mutated, action, sizeof(action));
	mutated[12] = 0;
	mutated[13] = 0;
	mutated[14] = 0;
	mutated[15] = 0x80;
	command = before;
	expect(!sol_ktx_decode_sac1_v1(mutated, sizeof(action), 42, 13, &command)
			&& !memcmp(&command, &before, sizeof(command)),
			"negative-zero SAC1 view fails canonical decoding atomically");
	memcpy(mutated, action, sizeof(action));
	mutated[32] = 1;
	mutated[33] = 2;
	mutated[34] = 0;
	mutated[35] = 'o';
	mutated[36] = 'k';
	command = before;
	expect(!sol_ktx_decode_sac1_v1(mutated, sizeof(mutated), 42, 13, &command)
			&& !memcmp(&command, &before, sizeof(command)),
			"canonical teamsay is rejected from the command emission phase");
}

static void test_plan_seat_evidence_identity_and_skill_token_stay_distinct(void)
{
	static const char *const plan_seats[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		"1", "2", "3", "4", "5", "6", "7", "8"
	};
	static const char *const evidence_seats[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		"candidate-1", "candidate-2", "candidate-3", "candidate-4",
		"control-5", "control-6", "control-7", "control-8"
	};
	static const char *const player_names[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		"cand-1", "cand-2", "cand-3", "cand-4",
		"ctrl-5", "ctrl-6", "ctrl-7", "ctrl-8"
	};
	static const char *const teams[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		"red", "red", "red", "red", "blue", "blue", "blue", "blue"
	};
	char evidence_seat[32];
	unsigned index;

	expect(SOL_KTX_CANDIDATE_COUNT_V1 == 4 && SOL_KTX_EVIDENCE_SEAT_COUNT_V1 == 8,
			"candidate registry remains four seats while evidence lifecycle is eight");
	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
	{
		const sol_ktx_seat_identity_v1 *identity =
				sol_ktx_plan_identity_v1(plan_seats[index]);

		expect(identity != NULL && identity->ordinal == index + 1u,
				"candidate plan seat has its fixed ordinal");
		expect(identity != NULL && !strcmp(identity->plan_seat, plan_seats[index])
				&& !strcmp(identity->evidence_seat, evidence_seats[index])
				&& !strcmp(identity->player_name, player_names[index])
				&& !strcmp(identity->team, teams[index])
				&& identity->role == (index < SOL_KTX_CANDIDATE_COUNT_V1 ?
					SOL_KTX_SEAT_CANDIDATE_V1 : SOL_KTX_SEAT_CONTROL_V1),
				"plan seat maps to exact role, evidence, bot, and team identities");
		expect(sol_ktx_plan_seat_v1(plan_seats[index], evidence_seat,
				sizeof(evidence_seat)) && !strcmp(evidence_seat, evidence_seats[index]),
				"evidence-seat projection covers all eight lifecycle seats");
	}
	expect(sol_ktx_plan_identity_v1("0") == NULL &&
			sol_ktx_plan_identity_v1("9") == NULL &&
			sol_ktx_plan_identity_v1("20") == NULL,
			"out-of-range and legacy skill tokens are never evidence seats");
	expect(!sol_ktx_plan_seat_v1("20", evidence_seat, sizeof(evidence_seat)),
			"legacy skill token 20 is not an evidencebind seat");
	expect(sol_ktx_add_shape_v1("20", "red"),
			"legacy addsol shape accepts skill token 20 and team red");
	expect(!sol_ktx_add_shape_v1("1", "red"),
			"numeric plan seat 1 is not the addsol skill token");
	expect(sol_ktx_control_selector_v1(20, "blue")
			&& !sol_ktx_control_selector_v1(20, "red")
			&& !sol_ktx_control_selector_v1(19, "blue"),
			"stock control selector is exactly addbot skill 20 team blue");
}

int main(void)
{
	test_first_frame_without_snapshot_is_exact_neutral();
	test_previous_self_snapshot_drives_exact_active_command();
	test_canonical_sac1_decodes_atomically();
	test_plan_seat_evidence_identity_and_skill_token_stay_distinct();
	if (!failures)
	{
		printf("sol_ktx_adapter: 4 contract tests passed\n");
	}
	return failures ? 1 : 0;
}
