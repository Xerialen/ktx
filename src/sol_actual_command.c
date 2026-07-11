#include "sol_actual_command.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "sol_ktx_adapter.h"

static int encode_exact_request(const sol_actual_command_input_v1 *input,
	uint32_t engine_slot, uint32_t client_generation,
	ce_frame_request_v1 *request)
{
	sol_ktx_command_v1 command;

	if (input->msec < 1 || input->msec > UINT8_MAX
			|| input->forwardmove < INT16_MIN || input->forwardmove > INT16_MAX
			|| input->sidemove < INT16_MIN || input->sidemove > INT16_MAX
			|| input->upmove < INT16_MIN || input->upmove > INT16_MAX
			|| input->buttons < 0 || input->buttons > UINT8_MAX
			|| input->impulse < 0 || input->impulse > UINT8_MAX)
	{
		return 0;
	}
	memset(&command, 0, sizeof(command));
	command.msec = (uint8_t) input->msec;
	memcpy(command.angles, input->angles, sizeof(command.angles));
	command.forwardmove = (int16_t) input->forwardmove;
	command.sidemove = (int16_t) input->sidemove;
	command.upmove = (int16_t) input->upmove;
	command.buttons = (uint8_t) input->buttons;
	command.impulse = (uint8_t) input->impulse;

	memset(request, 0, sizeof(*request));
	request->header.protocol_version = CE_PROTOCOL_VERSION_V1;
	request->header.struct_size = sizeof(*request);
	request->engine_slot = engine_slot;
	request->client_generation = client_generation;
	return sol_ktx_encode_command_v1(&command,
		request->requested_command.bytes);
}

static sol_actual_command_route_v1 lookup_route(
	const sol_actual_command_input_v1 *input,
	const sol_actual_command_ops_v1 *ops, uint32_t *engine_slot,
	uint32_t *client_generation)
{
	*engine_slot = 0u;
	*client_generation = 0u;
	if (!ops->lookup || input->engine_slot < 0 ||
		(uintmax_t) input->engine_slot > UINT32_MAX)
	{
		return SOL_ACTUAL_COMMAND_UNBOUND;
	}
	*engine_slot = (uint32_t) input->engine_slot;
	return ops->lookup(ops->context, *engine_slot, client_generation);
}

static sol_actual_command_request_status_v1 request_exact_route(
	const sol_actual_command_input_v1 *input,
	ce_operation_v1 operation, const sol_actual_command_ops_v1 *ops,
	sol_actual_command_route_v1 route, uint32_t engine_slot,
	uint32_t client_generation)
{
	ce_frame_request_v1 request;

	if (route != SOL_ACTUAL_COMMAND_BOUND || !ops->evidence ||
		(operation != CE_FRAME_REQUEST && operation != CE_FRAME_REPLACE) ||
		!encode_exact_request(input, engine_slot, client_generation, &request))
	{
		return SOL_ACTUAL_COMMAND_REQUEST_NOT_RUN;
	}
	return ops->evidence(ops->context, operation, &request) == CE_RESULT_OK ?
		SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED :
		SOL_ACTUAL_COMMAND_REQUEST_REJECTED;
}

static void safe_zero_neutral(const sol_actual_command_input_v1 *input,
	sol_actual_command_input_v1 *neutral)
{
	*neutral = *input;
	neutral->msec = input->msec >= 1 && input->msec <= UINT8_MAX ?
		input->msec : 1;
	neutral->angles[0] = 0.0f;
	neutral->angles[1] = 0.0f;
	neutral->angles[2] = 0.0f;
	neutral->forwardmove = 0;
	neutral->sidemove = 0;
	neutral->upmove = 0;
	neutral->buttons = 0;
	neutral->impulse = 0;
}

static int canonical_neutral(const sol_actual_command_input_v1 *input,
	const sol_actual_command_input_v1 *neutral)
{
	size_t index;

	if (!input || !neutral || neutral->engine_slot != input->engine_slot ||
		neutral->msec != input->msec || neutral->msec < 1 ||
		neutral->msec > UINT8_MAX || neutral->forwardmove != 0 ||
		neutral->sidemove != 0 || neutral->upmove != 0 ||
		neutral->buttons != 0 || neutral->impulse != 0)
	{
		return 0;
	}
	for (index = 0u; index < 3u; ++index)
	{
		if (!isfinite(neutral->angles[index]) ||
			(neutral->angles[index] == 0.0f && signbit(neutral->angles[index])))
		{
			return 0;
		}
	}
	return 1;
}

static void fail_after_neutral(const sol_actual_command_ops_v1 *ops)
{
	if (ops->fail_stop)
	{
		ops->fail_stop(ops->context);
	}
}

int sol_actual_command_submit_batch_v1(const sol_actual_command_input_v1 *inputs,
	const sol_actual_command_input_v1 *neutral_inputs,
	const ce_operation_v1 *operations, size_t count,
	const sol_actual_command_ops_v1 *ops,
	sol_actual_command_batch_result_v1 *results)
{
	sol_actual_command_input_v1 commands[SOL_ACTUAL_COMMAND_BATCH_MAX_V1];
	size_t index;
	int all_accepted = 1;
	int fallbacks_valid = 1;

	if (results && count <= SOL_ACTUAL_COMMAND_BATCH_MAX_V1)
	{
		memset(results, 0, count * sizeof(results[0]));
	}
	if (!inputs || !neutral_inputs || !operations || !count ||
		count > SOL_ACTUAL_COMMAND_BATCH_MAX_V1 || !ops || !ops->actual)
	{
		return 0;
	}
	for (index = 0u; index < count; ++index)
	{
		if (canonical_neutral(&inputs[index], &neutral_inputs[index]))
		{
			commands[index] = neutral_inputs[index];
		}
		else
		{
			safe_zero_neutral(&inputs[index], &commands[index]);
			fallbacks_valid = 0;
		}
	}
	all_accepted = fallbacks_valid;
	for (index = 0u; index < count; ++index)
	{
		sol_actual_command_route_v1 route;
		uint32_t client_generation;
		uint32_t engine_slot;
		sol_actual_command_request_status_v1 status =
			SOL_ACTUAL_COMMAND_REQUEST_NOT_RUN;

		route = lookup_route(&inputs[index], ops, &engine_slot,
			&client_generation);
		if (fallbacks_valid)
		{
			status = request_exact_route(&inputs[index], operations[index], ops,
				route, engine_slot, client_generation);
		}
		if (status == SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED)
		{
			commands[index] = inputs[index];
		}
		else
		{
			all_accepted = 0;
		}
		if (results)
		{
			results[index].request_status = status;
		}
	}
	if (!all_accepted)
	{
		for (index = 0u; index < count; ++index)
		{
			commands[index] = canonical_neutral(&inputs[index],
				&neutral_inputs[index]) ? neutral_inputs[index] : commands[index];
		}
	}
	for (index = 0u; index < count; ++index)
	{
		intptr_t actual_result = ops->actual(ops->context, &commands[index]);

		if (results)
		{
			results[index].actual_result = actual_result;
			results[index].emitted = 1;
		}
	}
	if (!all_accepted)
	{
		fail_after_neutral(ops);
	}
	return all_accepted;
}

intptr_t sol_actual_command_submit_v1(const sol_actual_command_input_v1 *input,
	ce_operation_v1 operation, const sol_actual_command_ops_v1 *ops)
{
	sol_actual_command_batch_result_v1 result;
	sol_actual_command_input_v1 neutral;
	sol_actual_command_route_v1 route;
	uint32_t client_generation;
	uint32_t engine_slot;

	if (!input || !ops || !ops->actual)
	{
		return 0;
	}
	route = lookup_route(input, ops, &engine_slot, &client_generation);
	if (route == SOL_ACTUAL_COMMAND_UNBOUND)
	{
		return ops->actual(ops->context, input);
	}
	if (request_exact_route(input, operation, ops, route, engine_slot,
			client_generation) == SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED)
	{
		return ops->actual(ops->context, input);
	}
	safe_zero_neutral(input, &neutral);
	result.actual_result = ops->actual(ops->context, &neutral);
	fail_after_neutral(ops);
	return result.actual_result;
}
