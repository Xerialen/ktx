#ifndef SOL_ACTUAL_COMMAND_H
#define SOL_ACTUAL_COMMAND_H

#include <stddef.h>
#include <stdint.h>

#include "controller_evidence_protocol.h"

typedef enum sol_actual_command_route_v1
{
	SOL_ACTUAL_COMMAND_UNBOUND = 0,
	SOL_ACTUAL_COMMAND_BOUND = 1,
	SOL_ACTUAL_COMMAND_AMBIGUOUS = 2,
	SOL_ACTUAL_COMMAND_QUARANTINED = 3
} sol_actual_command_route_v1;

typedef struct sol_actual_command_input_v1
{
	intptr_t engine_slot;
	intptr_t msec;
	float angles[3];
	intptr_t forwardmove;
	intptr_t sidemove;
	intptr_t upmove;
	intptr_t buttons;
	intptr_t impulse;
} sol_actual_command_input_v1;

typedef sol_actual_command_route_v1 (*sol_actual_command_lookup_v1)(
	void *context, uint32_t engine_slot, uint32_t *client_generation);
typedef intptr_t (*sol_actual_command_evidence_v1)(void *context,
	ce_operation_v1 operation,
	const ce_frame_request_v1 *request);
typedef intptr_t (*sol_actual_command_syscall_v1)(void *context,
	const sol_actual_command_input_v1 *command);
typedef void (*sol_actual_command_fail_stop_v1)(void *context);

typedef struct sol_actual_command_ops_v1
{
	void *context;
	sol_actual_command_lookup_v1 lookup;
	sol_actual_command_evidence_v1 evidence;
	sol_actual_command_syscall_v1 actual;
	sol_actual_command_fail_stop_v1 fail_stop;
} sol_actual_command_ops_v1;

#define SOL_ACTUAL_COMMAND_BATCH_MAX_V1 4u

typedef enum sol_actual_command_request_status_v1
{
	SOL_ACTUAL_COMMAND_REQUEST_REJECTED = -1,
	SOL_ACTUAL_COMMAND_REQUEST_NOT_RUN = 0,
	SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED = 1
} sol_actual_command_request_status_v1;

typedef struct sol_actual_command_batch_result_v1
{
	sol_actual_command_request_status_v1 request_status;
	intptr_t actual_result;
	int emitted;
} sol_actual_command_batch_result_v1;

/*
 * Requests every item before any physical syscall.  One rejected item makes
 * every physical command fresh neutral; request_status remains per-item truth.
 */
int sol_actual_command_submit_batch_v1(const sol_actual_command_input_v1 *inputs,
	const sol_actual_command_input_v1 *neutral_inputs,
	const ce_operation_v1 *operations, size_t count,
	const sol_actual_command_ops_v1 *ops,
	sol_actual_command_batch_result_v1 *results);
intptr_t sol_actual_command_submit_v1(const sol_actual_command_input_v1 *input,
	ce_operation_v1 operation, const sol_actual_command_ops_v1 *ops);

#endif
