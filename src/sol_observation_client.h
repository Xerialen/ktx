#ifndef SOL_OBSERVATION_CLIENT_H
#define SOL_OBSERVATION_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "controller_observation_protocol.h"

typedef struct sol_observation_client_v1 sol_observation_client_v1;

typedef intptr_t (*sol_observation_call_v1)(void *context, intptr_t operation,
	void *payload, intptr_t payload_size);

typedef enum sol_observation_status_v1 {
	SOL_OBSERVATION_INVALID = COV_RESULT_INVALID,
	SOL_OBSERVATION_EMPTY = COV_RESULT_EMPTY,
	SOL_OBSERVATION_READY = COV_RESULT_OK
} sol_observation_status_v1;

sol_observation_client_v1 *sol_observation_client_create_v1(
	uint32_t engine_slot, uint32_t client_generation,
	sol_observation_call_v1 call, void *call_context);
void sol_observation_client_destroy_v1(sol_observation_client_v1 *client);

const cov_profile_v1 *sol_observation_client_profile_v1(
	const sol_observation_client_v1 *client);
sol_observation_status_v1 sol_observation_client_poll_v1(
	sol_observation_client_v1 *client, uint32_t dt_us);
const uint8_t *sol_observation_client_batch_v1(
	const sol_observation_client_v1 *client, size_t *length);
int sol_observation_client_neutral_view_v1(
	const sol_observation_client_v1 *client, float output[3]);
uint64_t sol_observation_client_next_frame_v1(
	const sol_observation_client_v1 *client);

#endif
