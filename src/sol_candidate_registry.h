#ifndef SOL_CANDIDATE_REGISTRY_H
#define SOL_CANDIDATE_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "sol_brain.h"
#include "sol_ktx_adapter.h"
#include "sol_observation_client.h"

typedef struct sol_candidate_registry_v1 sol_candidate_registry_v1;

typedef enum sol_candidate_stage_v1
{
	SOL_CANDIDATE_EMPTY = 0,
	SOL_CANDIDATE_PENDING,
	SOL_CANDIDATE_EXPECTING_CLIENT,
	SOL_CANDIDATE_CLAIMED,
	SOL_CANDIDATE_BOUND
} sol_candidate_stage_v1;

typedef enum sol_candidate_decision_status_v1
{
	SOL_CANDIDATE_DECISION_REJECTED = -1,
	SOL_CANDIDATE_DECISION_NOT_RUN = 0,
	SOL_CANDIDATE_DECISION_SUBMITTED = 1
} sol_candidate_decision_status_v1;

typedef enum sol_candidate_request_status_v1
{
	SOL_CANDIDATE_REQUEST_REJECTED = -1,
	SOL_CANDIDATE_REQUEST_NOT_RUN = 0,
	SOL_CANDIDATE_REQUEST_ACCEPTED = 1
} sol_candidate_request_status_v1;

typedef struct sol_candidate_entry_view_v1
{
	size_t index;
	const sol_ktx_seat_identity_v1 *identity;
	sol_candidate_stage_v1 stage;
	int entity;
	uint32_t client_generation;
} sol_candidate_entry_view_v1;

typedef struct sol_candidate_frame_result_v1
{
	size_t index;
	int entity;
	uint32_t client_generation;
	sol_observation_status_v1 observation_status;
	sol_brain_status_v1 brain_status;
	size_t batch_length;
	size_t action_length;
	size_t trace_length;
	sol_candidate_decision_status_v1 decision_status;
	sol_candidate_request_status_v1 request_status;
	int frame_cancelled;
	int emitted;
} sol_candidate_frame_result_v1;

typedef int (*sol_candidate_healthy_v1)(void *context, size_t index,
	int entity, uint32_t client_generation);
typedef int (*sol_candidate_decision_v1)(void *context, size_t index,
	int entity, uint32_t client_generation, const uint8_t *action_response,
	size_t action_response_length, const uint8_t *decision_trace,
	size_t decision_trace_length);
typedef struct sol_candidate_command_batch_item_v1
{
	size_t index;
	int entity;
	sol_ktx_command_v1 command;
	sol_ktx_command_v1 neutral_command;
} sol_candidate_command_batch_item_v1;

typedef struct sol_candidate_command_batch_result_v1
{
	sol_candidate_request_status_v1 request_status;
	int emitted;
} sol_candidate_command_batch_result_v1;

typedef int (*sol_candidate_command_batch_v1)(void *context,
	const sol_candidate_command_batch_item_v1 *items, size_t count,
	sol_candidate_command_batch_result_v1 *results);
typedef void (*sol_candidate_remove_v1)(void *context, size_t index,
	int entity);

typedef struct sol_candidate_frame_ops_v1
{
	void *context;
	sol_candidate_healthy_v1 healthy;
	sol_candidate_decision_v1 decision;
	sol_candidate_command_batch_v1 commands;
} sol_candidate_frame_ops_v1;

sol_candidate_registry_v1 *sol_candidate_registry_create_v1(void);
void sol_candidate_registry_destroy_v1(sol_candidate_registry_v1 *registry);

int sol_candidate_registry_set_pending_v1(sol_candidate_registry_v1 *registry,
	size_t index);
int sol_candidate_registry_expect_client_v1(sol_candidate_registry_v1 *registry,
	size_t index);
int sol_candidate_registry_cancel_expect_v1(sol_candidate_registry_v1 *registry,
	size_t index);
int sol_candidate_registry_claim_v1(sol_candidate_registry_v1 *registry,
	const char *player_name, int entity, size_t *claimed_index);
int sol_candidate_registry_bind_v1(sol_candidate_registry_v1 *registry,
	size_t index, uint32_t client_generation, uint32_t stuck_replan_ms,
	sol_observation_call_v1 call, void *call_context);
int sol_candidate_registry_unbind_v1(sol_candidate_registry_v1 *registry,
	size_t index);
int sol_candidate_registry_release_v1(sol_candidate_registry_v1 *registry,
	size_t index);
size_t sol_candidate_registry_remove_all_v1(sol_candidate_registry_v1 *registry,
	sol_candidate_remove_v1 remove, void *remove_context);

int sol_candidate_registry_find_entity_v1(const sol_candidate_registry_v1 *registry,
	int entity, size_t *index);
int sol_candidate_registry_entry_v1(const sol_candidate_registry_v1 *registry,
	size_t index, sol_candidate_entry_view_v1 *output);
const cov_profile_v1 *sol_candidate_registry_profile_v1(
	const sol_candidate_registry_v1 *registry, size_t index);

size_t sol_candidate_registry_run_frame_v1(sol_candidate_registry_v1 *registry,
	uint8_t msec, uint32_t dt_us, const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1]);

#endif
