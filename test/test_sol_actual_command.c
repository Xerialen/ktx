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
	intptr_t actual_result;
	sol_actual_command_input_v1 expected_actual;
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

	require(engine_slot == fixture->expected_slot,
			"binding lookup receives the exact original engine slot");
	fixture->lookup_calls++;
	if (client_generation)
	{
		*client_generation = fixture->route == SOL_ACTUAL_COMMAND_BOUND ?
			fixture->generation : 0u;
	}
	return fixture->route;
}

static intptr_t write_evidence(void *context, ce_operation_v1 operation,
	const ce_frame_request_v1 *request)
{
	command_fixture_v1 *fixture = context;

	trace(fixture, 'E');
	fixture->evidence_calls++;
	require(operation == fixture->expected_operation
		&& request != NULL
		&& request->header.protocol_version == CE_PROTOCOL_VERSION_V1
		&& request->header.struct_size == sizeof(*request)
		&& request->engine_slot == fixture->expected_slot
		&& request->client_generation == fixture->generation
		&& !memcmp(request->requested_command.bytes, fixture->expected_wire,
			sizeof(fixture->expected_wire)),
			"evidence request seals the exact original representable command");
	return fixture->evidence_result;
}

static intptr_t write_actual(void *context,
	const sol_actual_command_input_v1 *command)
{
	command_fixture_v1 *fixture = context;

	trace(fixture, 'A');
	fixture->actual_calls++;
	require(command != NULL
		&& command->engine_slot == fixture->expected_actual.engine_slot
		&& command->msec == fixture->expected_actual.msec
		&& !memcmp(command->angles, fixture->expected_actual.angles,
			sizeof(command->angles))
		&& command->forwardmove == fixture->expected_actual.forwardmove
		&& command->sidemove == fixture->expected_actual.sidemove
		&& command->upmove == fixture->expected_actual.upmove
		&& command->buttons == fixture->expected_actual.buttons
		&& command->impulse == fixture->expected_actual.impulse,
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

static void test_evidence_failure_calls_actual_once_then_fail_stops(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops;

	fixture.evidence_result = CE_RESULT_INVALID;
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "EAF") && fixture.evidence_calls == 1
		&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
			"evidence rejection cannot suppress actual syscall and fails afterward");
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

		require(sol_actual_command_submit_v1(&invalid[index], CE_FRAME_REQUEST,
			&ops) == 77
			&& !strcmp(fixture.trace, "AF") && fixture.evidence_calls == 0
			&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
				"non-representable bound command is actual once then fail-stop");
	}
}

static void test_ambiguous_binding_calls_actual_once_then_fail_stops(void)
{
	sol_actual_command_input_v1 input = valid_command();
	command_fixture_v1 fixture = fixture_for(&input);
	sol_actual_command_ops_v1 ops;

	fixture.route = SOL_ACTUAL_COMMAND_AMBIGUOUS;
	ops = ops_for(&fixture);
	require(sol_actual_command_submit_v1(&input, CE_FRAME_REQUEST, &ops) == 77
		&& !strcmp(fixture.trace, "AF") && fixture.evidence_calls == 0
		&& fixture.actual_calls == 1 && fixture.fail_calls == 1,
			"ambiguous successful routes never fabricate one evidence owner");
}

int main(void)
{
	test_bound_valid_command_requests_then_calls_actual_once();
	test_bound_replacement_routes_explicit_evidence_operation();
	test_unbound_command_is_byte_and_behavior_bypass();
	test_evidence_failure_calls_actual_once_then_fail_stops();
	test_invalid_bound_commands_call_actual_once_without_fabricated_request();
	test_ambiguous_binding_calls_actual_once_then_fail_stops();
	printf("sol_actual_command: 6 wrapper contracts passed\n");
	return 0;
}
