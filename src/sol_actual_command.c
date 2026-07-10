#include "sol_actual_command.h"

#include <limits.h>
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

static void fail_after_actual(const sol_actual_command_ops_v1 *ops)
{
	if (ops->fail_stop)
	{
		ops->fail_stop(ops->context);
	}
}

intptr_t sol_actual_command_submit_v1(const sol_actual_command_input_v1 *input,
	ce_operation_v1 operation, const sol_actual_command_ops_v1 *ops)
{
	sol_actual_command_route_v1 route = SOL_ACTUAL_COMMAND_UNBOUND;
	ce_frame_request_v1 request;
	uint32_t client_generation = 0;
	uint32_t engine_slot = 0;
	intptr_t evidence_result = CE_RESULT_INVALID;
	intptr_t actual_result;

	if (!input || !ops || !ops->actual)
	{
		return 0;
	}
	if (ops->lookup && input->engine_slot >= 0
			&& (uintmax_t) input->engine_slot <= UINT32_MAX)
	{
		engine_slot = (uint32_t) input->engine_slot;
		route = ops->lookup(ops->context, engine_slot, &client_generation);
	}
	if (route == SOL_ACTUAL_COMMAND_BOUND && ops->evidence
			&& (operation == CE_FRAME_REQUEST || operation == CE_FRAME_REPLACE)
			&& encode_exact_request(input, engine_slot, client_generation, &request))
	{
		evidence_result = ops->evidence(ops->context, operation, &request);
		actual_result = ops->actual(ops->context, input);
		if (evidence_result != CE_RESULT_OK)
		{
			fail_after_actual(ops);
		}
		return actual_result;
	}

	actual_result = ops->actual(ops->context, input);
	if (route != SOL_ACTUAL_COMMAND_UNBOUND)
	{
		fail_after_actual(ops);
	}
	return actual_result;
}
