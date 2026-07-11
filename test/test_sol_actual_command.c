#include "sol_actual_command.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct command_fixture_v1
{
	sol_actual_command_route_v1 route;
	uint32_t expected_slot;
	uint32_t generation;
	ce_operation_v1 expected_operation;
	int evidence_result;
	int reject_evidence_call;
	intptr_t actual_result;
	sol_actual_command_input_v1 expected_actual;
	sol_actual_command_input_v1 batch_actual[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	uint32_t batch_slots[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	uint32_t batch_generations[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	size_t batch_count;
	uint8_t expected_wire[CE_COMMAND_BYTES_V1_SIZE];
	int lookup_calls;
	int evidence_calls;
	int actual_calls;
	int fail_calls;
	char trace[16];
	size_t trace_length;
} command_fixture_v1;

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void trace(command_fixture_v1 *fixture, char operation)
{
	require(fixture->trace_length + 1u < sizeof(fixture->trace),
			"command trace remains bounded");
	fixture->trace[fixture->trace_length++] = operation;
	fixture->trace[fixture->trace_length] = '\0';
}

static sol_actual_command_route_v1 lookup_route(void *context,
	uint32_t engine_slot, uint32_t *client_generation)
{
	command_fixture_v1 *fixture = context;
	size_t call = (size_t) fixture->lookup_calls;
	uint32_t expected_slot = fixture->batch_count ? fixture->batch_slots[call] :
		fixture->expected_slot;

	require(call < (fixture->batch_count ? fixture->batch_count : 1u) &&
		engine_slot == expected_slot,
			"binding lookup receives the exact original engine slot");
	fixture->lookup_calls++;
	if (client_generation)
	{
		*client_generation = fixture->route == SOL_ACTUAL_COMMAND_BOUND ?
			(fixture->batch_count ? fixture->batch_generations[call] :
				fixture->generation) : 0u;
	}
	return fixture->route;
}

static intptr_t write_evidence(void *context, ce_operation_v1 operation,
	const ce_frame_request_v1 *request)
{
	command_fixture_v1 *fixture = context;
	size_t call = (size_t) fixture->evidence_calls;
	uint32_t expected_slot = fixture->batch_count ? fixture->batch_slots[call] :
		fixture->expected_slot;
	uint32_t expected_generation = fixture->batch_count ?
		fixture->batch_generations[call] : fixture->generation;

	trace(fixture, 'E');
	fixture->evidence_calls++;
	require(operation == fixture->expected_operation
		&& request != NULL
		&& request->header.protocol_version == CE_PROTOCOL_VERSION_V1
		&& request->header.struct_size == sizeof(*request)
		&& call < (fixture->batch_count ? fixture->batch_count : 1u)
		&& request->engine_slot == expected_slot
		&& request->client_generation == expected_generation
		&& !memcmp(request->requested_command.bytes, fixture->expected_wire,
			sizeof(fixture->expected_wire)),
			"evidence request seals the exact original representable command");
	return fixture->reject_evidence_call == fixture->evidence_calls ?
		CE_RESULT_INVALID : fixture->evidence_result;
}

static intptr_t write_actual(void *context,
	const sol_actual_command_input_v1 *command)
{
	command_fixture_v1 *fixture = context;
	size_t call = (size_t) fixture->actual_calls;
	const sol_actual_command_input_v1 *expected = fixture->batch_count ?
		&fixture->batch_actual[call] : &fixture->expected_actual;

	trace(fixture, 'A');
	fixture->actual_calls++;
	require(command != NULL
		&& call < (fixture->batch_count ? fixture->batch_count : 1u)
		&& command->engine_slot == expected->engine_slot
		&& command->msec == expected->msec
		&& !memcmp(command->angles, expected->angles,
			sizeof(command->angles))
		&& command->forwardmove == expected->forwardmove
		&& command->sidemove == expected->sidemove
		&& command->upmove == expected->upmove
		&& command->buttons == expected->buttons
		&& command->impulse == expected->impulse,
			"real syscall receives every original argument without normalization");
	return fixture->actual_result;
}

static void mark_fail_stop(void *context)
{
	command_fixture_v1 *fixture = context;

	trace(fixture, 'F');
	fixture->fail_calls++;
}

static sol_actual_command_input_v1 valid_command(void)
{
	sol_actual_command_input_v1 input;

	memset(&input, 0, sizeof(input));
	input.engine_slot = 7;
	input.msec = 13;
	input.angles[0] = 0.0f;
	input.angles[1] = 45.0f;
	input.angles[2] = -180.0f;
	input.forwardmove = 400;
	input.sidemove = -32768;
	input.upmove = 32767;
	input.buttons = 255;
	input.impulse = 0;
	return input;
}

static command_fixture_v1 fixture_for(
	const sol_actual_command_input_v1 *input)
{
	static const uint8_t expected_wire[CE_COMMAND_BYTES_V1_SIZE] = {
		'S', 'U', 'C', '1', 13,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x34, 0x42,
		0x00, 0x00, 0x34, 0xc3,
		0x90, 0x01, 0x00, 0x80, 0xff, 0x7f, 0xff, 0x00
	};
	command_fixture_v1 fixture;

	memset(&fixture, 0, sizeof(fixture));
	fixture.route = SOL_ACTUAL_COMMAND_BOUND;
	fixture.expected_slot = 7u;
	fixture.generation = 42u;
	fixture.expected_operation = CE_FRAME_REQUEST;
	fixture.evidence_result = CE_RESULT_OK;
	fixture.actual_result = 77;
	fixture.expected_actual = *input;
	memcpy(fixture.expected_wire, expected_wire, sizeof(expected_wire));
	return fixture;
}

static sol_actual_command_ops_v1 ops_for(command_fixture_v1 *fixture)
{
	sol_actual_command_ops_v1 ops = {
		fixture, lookup_route, write_evidence, write_actual, mark_fail_stop
	};

	return ops;
}

static void expect_fresh_neutral(command_fixture_v1 *fixture,
	const sol_actual_command_input_v1 *input)
{
	fixture->expected_actual = *input;
	fixture->expected_actual.forwardmove = 0;
	fixture->expected_actual.sidemove = 0;
	fixture->expected_actual.upmove = 0;
	fixture->expected_actual.buttons = 0;
	fixture->expected_actual.impulse = 0;
	fixture->expected_actual.angles[0] = 0.0f;
	fixture->expected_actual.angles[1] = 0.0f;
	fixture->expected_actual.angles[2] = 0.0f;
	if (fixture->expected_actual.msec < 1 || fixture->expected_actual.msec > 255)
	{
		fixture->expected_actual.msec = 1;
	}
}

static void test_bound_valid_command_requests_then_calls_actual_once(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops = ops_for(&fixture);

	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "EA") && fixture.lookup_calls == 1
		&& fixture.evidence_calls == 1 && fixture.actual_calls == 1
		&& fixture.fail_calls == 0,
			"bound representable command requests immediately before one real syscall");
}

static void test_bound_replacement_routes_explicit_evidence_operation(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops = ops_for(&fixture);

	fixture.expected_operation = CE_FRAME_REPLACE;
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REPLACE, &ops) == 77
		&& !strcmp(fixture.trace, "EA") && fixture.evidence_calls == 1
		&& fixture.actual_calls == 1 && fixture.fail_calls == 0,
			"declared blocked replacement reaches the distinct evidence operation");
}

static void test_candidate_batch_requests_all_before_any_physical_emit(void)
{
	sol_actual_command_input_v1 inputs[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	sol_actual_command_input_v1 neutrals[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	ce_operation_v1 operations[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	sol_actual_command_batch_result_v1 results[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture;
	sol_actual_command_ops_v1 ops;
	size_t index;

	fixture = fixture_for(&input);
	ops = ops_for(&fixture);
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		inputs[index] = input;
		inputs[index].engine_slot = (intptr_t) (7u + index);
		neutrals[index] = inputs[index];
		neutrals[index].angles[0] = 1.0f + (float) index;
		neutrals[index].angles[1] = 10.0f + (float) index;
		neutrals[index].angles[2] = 0.0f;
		neutrals[index].forwardmove = 0;
		neutrals[index].sidemove = 0;
		neutrals[index].upmove = 0;
		neutrals[index].buttons = 0;
		neutrals[index].impulse = 0;
		operations[index] = CE_FRAME_REQUEST;
		fixture.batch_slots[index] = (uint32_t) inputs[index].engine_slot;
		fixture.batch_generations[index] = 42u + (uint32_t) index;
		fixture.batch_actual[index] = inputs[index];
	}
	fixture.batch_count = SOL_ACTUAL_COMMAND_BATCH_MAX_V1;
	require(sol_actual_command_submit_batch_v1(inputs, neutrals, operations,
			SOL_ACTUAL_COMMAND_BATCH_MAX_V1, &ops, results)
		&& !strcmp(fixture.trace, "EEEEAAAA") && fixture.lookup_calls == 4
		&& fixture.evidence_calls == 4 && fixture.actual_calls == 4
		&& fixture.fail_calls == 0,
			"candidate batch requests every command before any physical emission");
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		require(results[index].request_status ==
				SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED && results[index].emitted,
				"successful batch reports every accepted request and physical write");
	}
	fixture = fixture_for(&input);
	fixture.reject_evidence_call = 2;
	fixture.batch_count = SOL_ACTUAL_COMMAND_BATCH_MAX_V1;
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		fixture.batch_slots[index] = (uint32_t) inputs[index].engine_slot;
		fixture.batch_generations[index] = 42u + (uint32_t) index;
		fixture.batch_actual[index] = neutrals[index];
	}
	ops = ops_for(&fixture);
	require(!sol_actual_command_submit_batch_v1(inputs, neutrals, operations,
			SOL_ACTUAL_COMMAND_BATCH_MAX_V1, &ops, results)
		&& !strcmp(fixture.trace, "EEEEAAAAF") && fixture.lookup_calls == 4
		&& fixture.evidence_calls == 4 && fixture.actual_calls == 4
		&& fixture.fail_calls == 1,
			"one request rejection neutralizes every physical command after all requests");
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		require(results[index].request_status ==
				(index == 1u ? SOL_ACTUAL_COMMAND_REQUEST_REJECTED :
					SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED) && results[index].emitted,
				"failed batch retains truthful per-request status without active leakage");
	}
	fixture = fixture_for(&input);
	fixture.route = SOL_ACTUAL_COMMAND_UNBOUND;
	fixture.batch_count = SOL_ACTUAL_COMMAND_BATCH_MAX_V1;
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		fixture.batch_slots[index] = (uint32_t) inputs[index].engine_slot;
		fixture.batch_actual[index] = neutrals[index];
	}
	ops = ops_for(&fixture);
	require(!sol_actual_command_submit_batch_v1(inputs, neutrals, operations,
			SOL_ACTUAL_COMMAND_BATCH_MAX_V1, &ops, results)
		&& !strcmp(fixture.trace, "AAAAF") && fixture.lookup_calls == 4
		&& fixture.evidence_calls == 0 && fixture.actual_calls == 4
		&& fixture.fail_calls == 1,
			"strict candidate batch never treats an unbound route as a bypass");
	for (index = 0u; index < SOL_ACTUAL_COMMAND_BATCH_MAX_V1; ++index)
	{
		require(results[index].request_status ==
				SOL_ACTUAL_COMMAND_REQUEST_NOT_RUN && results[index].emitted,
				"every unbound candidate route is explicit and physically neutral");
	}
}

static void test_unbound_command_is_byte_and_behavior_bypass(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture;
	sol_actual_command_ops_v1 ops;

	input.msec = 1000;
	input.angles[0] = -0.0f;
	input.forwardmove = 99999;
	input.buttons = -1;
	fixture = fixture_for(&input);
	fixture.route = SOL_ACTUAL_COMMAND_UNBOUND;
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "A") && fixture.lookup_calls == 1
		&& fixture.evidence_calls == 0 && fixture.actual_calls == 1
		&& fixture.fail_calls == 0,
			"unbound command bypasses evidence and validation without changing bytes");
}

static void test_evidence_failure_emits_neutral_once_then_fail_stops(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops;

	fixture.evidence_result = CE_RESULT_INVALID;
	expect_fresh_neutral(&fixture, &input);
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "EAF") && fixture.evidence_calls == 1
		&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
			"evidence rejection physically emits only fresh neutral then fail-stops");
}

static void test_invalid_bound_commands_call_actual_once_without_fabricated_request(void)
{
	sol_actual_command_input_v1 invalid[10];
	size_t index;

	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
	{
		invalid[index] = valid_command();
	}
	invalid[0].msec = 0;
	invalid[1].msec = 256;
	invalid[2].forwardmove = 32768;
	invalid[3].sidemove = -32769;
	invalid[4].upmove = 32768;
	invalid[5].buttons = 256;
	invalid[6].buttons = -1;
	invalid[7].impulse = 256;
	invalid[8].angles[1] = NAN;
	invalid[9].angles[2] = -0.0f;
	for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index)
	{
		command_fixture_v1 fixture = fixture_for(&invalid[index]);
		sol_actual_command_ops_v1 ops = ops_for(&fixture);

		expect_fresh_neutral(&fixture, &invalid[index]);

		require(sol_actual_command_submit_v1(&invalid[index], CE_FRAME_REQUEST,
			&ops) == 77
			&& !strcmp(fixture.trace, "AF") && fixture.evidence_calls == 0
			&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
				"non-representable bound command emits sanitized neutral then fail-stop");
	}
}

static void test_ambiguous_binding_calls_actual_once_then_fail_stops(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops;

	fixture.route = SOL_ACTUAL_COMMAND_AMBIGUOUS;
	expect_fresh_neutral(&fixture, &input);
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "AF") && fixture.evidence_calls == 0
		&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
			"ambiguous successful routes never fabricate one evidence owner");
}

static void test_quarantined_client_emits_neutral_without_evidence(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops;

	fixture.route = SOL_ACTUAL_COMMAND_QUARANTINED;
	expect_fresh_neutral(&fixture, &input);
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "AF") && fixture.evidence_calls == 0
		&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
			"owned client without an open exact evidence route stays physically neutral");
}

int main(void)
{
	test_bound_valid_command_requests_then_calls_actual_once();
	test_bound_replacement_routes_explicit_evidence_operation();
	test_candidate_batch_requests_all_before_any_physical_emit();
	test_unbound_command_is_byte_and_behavior_bypass();
	test_evidence_failure_emits_neutral_once_then_fail_stops();
	test_invalid_bound_commands_call_actual_once_without_fabricated_request();
	test_ambiguous_binding_calls_actual_once_then_fail_stops();
	test_quarantined_client_emits_neutral_without_evidence();
	printf("sol_actual_command: 8 wrapper contracts passed\n");
	return 0;
}
