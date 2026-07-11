#include "sol_candidate_registry.h"

#include "sol_decision_trace.h"

#include <stdlib.h>
#include <string.h>

#define SOL_CANDIDATE_MAX_ENGINE_SLOT_V1 32

typedef struct sol_candidate_entry_v1
{
	sol_candidate_stage_v1 stage;
	int entity;
	uint32_t client_generation;
	sol_observation_client_v1 *observation;
	sol_brain_v1 *brain;
	int initial_empty_consumed;
} sol_candidate_entry_v1;

typedef struct sol_candidate_prepared_v1
{
	int participating;
	int emittable;
	int bound_unhealthy;
	int observation_lifecycle_invalid;
	sol_observation_status_v1 observation_status;
	sol_brain_status_v1 brain_status;
	size_t batch_length;
	sol_brain_decision_view_v1 decision;
	int decision_ready;
	sol_candidate_decision_status_v1 decision_status;
	sol_ktx_command_v1 command;
	sol_ktx_command_v1 neutral_command;
} sol_candidate_prepared_v1;

struct sol_candidate_registry_v1
{
	sol_candidate_entry_v1 entries[SOL_KTX_CANDIDATE_COUNT_V1];
};

static const sol_ktx_seat_identity_v1 *identity_at(size_t index)
{
	static const char *const plan_seats[SOL_KTX_CANDIDATE_COUNT_V1] = {
		"1", "2", "3", "4"
	};

	return index < SOL_KTX_CANDIDATE_COUNT_V1 ?
		sol_ktx_plan_identity_v1(plan_seats[index]) : NULL;
}

static int valid_index(size_t index)
{
	return index < SOL_KTX_CANDIDATE_COUNT_V1 && identity_at(index) != NULL;
}

sol_candidate_registry_v1 *sol_candidate_registry_create_v1(void)
{
	sol_candidate_registry_v1 *registry;
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		if (!identity_at(index))
		{
			return NULL;
		}
	}
	registry = calloc(1u, sizeof(*registry));
	return registry;
}

void sol_candidate_registry_destroy_v1(sol_candidate_registry_v1 *registry)
{
	size_t index;

	if (!registry)
	{
		return;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_brain_close_v1(registry->entries[index].brain);
		sol_observation_client_destroy_v1(registry->entries[index].observation);
	}
	memset(registry, 0, sizeof(*registry));
	free(registry);
}

int sol_candidate_registry_set_pending_v1(sol_candidate_registry_v1 *registry,
	size_t index)
{
	if (!registry || !valid_index(index) ||
		registry->entries[index].stage != SOL_CANDIDATE_EMPTY)
	{
		return 0;
	}
	registry->entries[index].stage = SOL_CANDIDATE_PENDING;
	return 1;
}

int sol_candidate_registry_expect_client_v1(sol_candidate_registry_v1 *registry,
	size_t index)
{
	size_t other;

	if (!registry || !valid_index(index) ||
		registry->entries[index].stage != SOL_CANDIDATE_PENDING)
	{
		return 0;
	}
	for (other = 0; other < SOL_KTX_CANDIDATE_COUNT_V1; ++other)
	{
		if (registry->entries[other].stage == SOL_CANDIDATE_EXPECTING_CLIENT)
		{
			return 0;
		}
	}
	registry->entries[index].stage = SOL_CANDIDATE_EXPECTING_CLIENT;
	return 1;
}

int sol_candidate_registry_cancel_expect_v1(sol_candidate_registry_v1 *registry,
	size_t index)
{
	if (!registry || !valid_index(index) ||
		registry->entries[index].stage != SOL_CANDIDATE_EXPECTING_CLIENT)
	{
		return 0;
	}
	registry->entries[index].stage = SOL_CANDIDATE_PENDING;
	return 1;
}

int sol_candidate_registry_claim_v1(sol_candidate_registry_v1 *registry,
	const char *player_name, int entity, size_t *claimed_index)
{
	size_t index;
	size_t expected = SOL_KTX_CANDIDATE_COUNT_V1;

	if (claimed_index)
	{
		*claimed_index = SOL_KTX_CANDIDATE_COUNT_V1;
	}
	if (!registry || !player_name || entity < 1 ||
		entity > SOL_CANDIDATE_MAX_ENGINE_SLOT_V1)
	{
		return 0;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_candidate_entry_v1 *entry = &registry->entries[index];

		if (entry->stage >= SOL_CANDIDATE_CLAIMED && entry->entity == entity)
		{
			return 0;
		}
		if (entry->stage == SOL_CANDIDATE_EXPECTING_CLIENT &&
			!strcmp(player_name, identity_at(index)->player_name))
		{
			if (expected != SOL_KTX_CANDIDATE_COUNT_V1)
			{
				return 0;
			}
			expected = index;
		}
	}
	if (expected == SOL_KTX_CANDIDATE_COUNT_V1)
	{
		return 0;
	}
	registry->entries[expected].entity = entity;
	registry->entries[expected].stage = SOL_CANDIDATE_CLAIMED;
	if (claimed_index)
	{
		*claimed_index = expected;
	}
	return 1;
}

int sol_candidate_registry_bind_v1(sol_candidate_registry_v1 *registry,
	size_t index, uint32_t client_generation, uint32_t stuck_replan_ms,
	sol_observation_call_v1 call, void *call_context)
{
	sol_candidate_entry_v1 *entry;
	sol_observation_client_v1 *observation;
	sol_brain_bootstrap_v1 bootstrap = {0};
	sol_brain_v1 *brain;
	const cov_profile_v1 *profile;

	if (!registry || !valid_index(index) || !client_generation
		|| stuck_replan_ms < SOL_BRAIN_STUCK_REPLAN_MIN_MS_V1
		|| stuck_replan_ms > SOL_BRAIN_STUCK_REPLAN_MAX_MS_V1 || !call)
	{
		return 0;
	}
	entry = &registry->entries[index];
	if (entry->stage != SOL_CANDIDATE_CLAIMED || entry->entity < 1 ||
		entry->observation)
	{
		return 0;
	}
	observation = sol_observation_client_create_v1((uint32_t) entry->entity,
		client_generation, call, call_context);
	if (!observation)
	{
		return 0;
	}
	profile = sol_observation_client_profile_v1(observation);
	if (!profile)
	{
		sol_observation_client_destroy_v1(observation);
		return 0;
	}
	bootstrap.struct_size = sizeof(bootstrap);
	bootstrap.stuck_replan_ms = stuck_replan_ms;
	memcpy(bootstrap.static_asset_set_id, profile->static_asset_set_id,
		sizeof(bootstrap.static_asset_set_id));
	memcpy(bootstrap.sensory_profile_id, profile->sensory_profile_id,
		sizeof(bootstrap.sensory_profile_id));
	brain = sol_brain_open_v1(&bootstrap);
	if (!brain)
	{
		sol_observation_client_destroy_v1(observation);
		return 0;
	}
	entry->client_generation = client_generation;
	entry->observation = observation;
	entry->brain = brain;
	entry->initial_empty_consumed = 0;
	entry->stage = SOL_CANDIDATE_BOUND;
	return 1;
}

int sol_candidate_registry_unbind_v1(sol_candidate_registry_v1 *registry,
	size_t index)
{
	sol_candidate_entry_v1 *entry;

	if (!registry || !valid_index(index))
	{
		return 0;
	}
	entry = &registry->entries[index];
	if (entry->stage == SOL_CANDIDATE_CLAIMED && !entry->observation &&
		!entry->client_generation)
	{
		return 1;
	}
	if (entry->stage != SOL_CANDIDATE_BOUND || !entry->observation || !entry->brain ||
		!entry->client_generation)
	{
		return 0;
	}
	sol_brain_close_v1(entry->brain);
	sol_observation_client_destroy_v1(entry->observation);
	entry->brain = NULL;
	entry->observation = NULL;
	entry->client_generation = 0u;
	entry->initial_empty_consumed = 0;
	entry->stage = SOL_CANDIDATE_CLAIMED;
	return 1;
}

int sol_candidate_registry_release_v1(sol_candidate_registry_v1 *registry,
	size_t index)
{
	sol_candidate_entry_v1 *entry;

	if (!registry || !valid_index(index))
	{
		return 0;
	}
	entry = &registry->entries[index];
	if (entry->stage == SOL_CANDIDATE_EMPTY)
	{
		return 0;
	}
	sol_brain_close_v1(entry->brain);
	sol_observation_client_destroy_v1(entry->observation);
	memset(entry, 0, sizeof(*entry));
	return 1;
}

size_t sol_candidate_registry_remove_all_v1(sol_candidate_registry_v1 *registry,
	sol_candidate_remove_v1 remove, void *remove_context)
{
	size_t index;
	size_t removed = 0u;

	if (!registry || !remove)
	{
		return 0u;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		int entity;

		if (registry->entries[index].stage < SOL_CANDIDATE_CLAIMED)
		{
			continue;
		}
		entity = registry->entries[index].entity;
		remove(remove_context, index, entity);
		removed++;
		/* Engine removal normally dispatches the disconnect hook synchronously.
		 * A host that does not must not leave an aliased registry entry behind. */
		if (registry->entries[index].stage != SOL_CANDIDATE_EMPTY)
		{
			sol_candidate_registry_release_v1(registry, index);
		}
	}
	return removed;
}

int sol_candidate_registry_find_entity_v1(const sol_candidate_registry_v1 *registry,
	int entity, size_t *index)
{
	size_t candidate;

	if (index)
	{
		*index = SOL_KTX_CANDIDATE_COUNT_V1;
	}
	if (!registry || entity < 1)
	{
		return 0;
	}
	for (candidate = 0; candidate < SOL_KTX_CANDIDATE_COUNT_V1; ++candidate)
	{
		const sol_candidate_entry_v1 *entry = &registry->entries[candidate];

		if (entry->stage >= SOL_CANDIDATE_CLAIMED && entry->entity == entity)
		{
			if (index)
			{
				*index = candidate;
			}
			return 1;
		}
	}
	return 0;
}

int sol_candidate_registry_entry_v1(const sol_candidate_registry_v1 *registry,
	size_t index, sol_candidate_entry_view_v1 *output)
{
	const sol_candidate_entry_v1 *entry;

	if (!registry || !valid_index(index) || !output)
	{
		return 0;
	}
	entry = &registry->entries[index];
	memset(output, 0, sizeof(*output));
	output->index = index;
	output->identity = identity_at(index);
	output->stage = entry->stage;
	output->entity = entry->entity;
	output->client_generation = entry->client_generation;
	return 1;
}

const cov_profile_v1 *sol_candidate_registry_profile_v1(
	const sol_candidate_registry_v1 *registry, size_t index)
{
	if (!registry || !valid_index(index) ||
		registry->entries[index].stage != SOL_CANDIDATE_BOUND)
	{
		return NULL;
	}
	return sol_observation_client_profile_v1(registry->entries[index].observation);
}

static void neutral_command(uint8_t msec,
	const sol_observation_client_v1 *observation, sol_ktx_command_v1 *command)
{
	memset(command, 0, sizeof(*command));
	command->msec = msec;
	sol_observation_client_neutral_view_v1(observation, command->angles);
}

static void poll_all_bound_v1(sol_candidate_registry_v1 *registry,
	uint32_t dt_us, const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1],
	sol_candidate_frame_result_v1 *results)
{
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_candidate_entry_v1 *entry = &registry->entries[index];
		sol_candidate_prepared_v1 *seat = &prepared[index];

		if (entry->stage != SOL_CANDIDATE_BOUND)
		{
			continue;
		}
		if (!entry->observation || !entry->brain ||
			!ops->healthy(ops->context, index, entry->entity,
				entry->client_generation))
		{
			seat->bound_unhealthy = 1;
			continue;
		}
		seat->participating = 1;
		seat->observation_status = sol_observation_client_poll_v1(
			entry->observation, dt_us);
		sol_observation_client_batch_v1(entry->observation,
			&seat->batch_length);
		/* Observation integrity is reported to the host, but a still-bound live
		 * seat must receive one fresh neutral writer pair in this outer frame. */
		seat->emittable = 1;
		if (seat->observation_status == SOL_OBSERVATION_EMPTY)
		{
			if (entry->initial_empty_consumed ||
				sol_observation_client_next_frame_v1(entry->observation) > 0u)
			{
				seat->observation_lifecycle_invalid = 1;
			}
			else
			{
				entry->initial_empty_consumed = 1;
			}
		}
		else if (seat->observation_status == SOL_OBSERVATION_READY &&
			!entry->initial_empty_consumed)
		{
			seat->observation_lifecycle_invalid = 1;
		}
		if (results)
		{
			results[index].observation_status = seat->observation_status;
			results[index].batch_length = seat->batch_length;
		}
	}
}

static void prepare_all_bound_v1(sol_candidate_registry_v1 *registry,
	uint8_t msec,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1],
	sol_candidate_frame_result_v1 *results)
{
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_candidate_entry_v1 *entry = &registry->entries[index];
		sol_candidate_prepared_v1 *seat = &prepared[index];

		if (!seat->participating || !seat->emittable)
		{
			continue;
		}
		neutral_command(msec, entry->observation, &seat->neutral_command);
		if (seat->observation_lifecycle_invalid &&
			!entry->initial_empty_consumed)
		{
			seat->neutral_command.angles[0] = 0.0f;
			seat->neutral_command.angles[1] = 0.0f;
			seat->neutral_command.angles[2] = 0.0f;
		}
		seat->command = seat->neutral_command;
		seat->brain_status = SOL_BRAIN_NEUTRAL;
		if (seat->observation_status == SOL_OBSERVATION_READY &&
			!seat->observation_lifecycle_invalid)
		{
			const uint8_t *batch;
			sol_brain_decision_view_v1 decision = {0};
			sol_ktx_command_v1 active_command;
			size_t batch_length = 0u;
			uint64_t next_frame;

			batch = sol_observation_client_batch_v1(entry->observation,
				&batch_length);
			seat->brain_status = sol_brain_decide_v1(entry->brain, batch,
				batch_length, &decision);
			next_frame = sol_observation_client_next_frame_v1(entry->observation);
			if (seat->brain_status >= SOL_BRAIN_NEUTRAL
				&& next_frame > 0u
				&& sol_ktx_decode_sac1_v1(decision.sac1,
					decision.sac1_length, next_frame - 1u, msec,
					&active_command)
				&& decision.sdt1 && decision.sdt1_length > 0u
				&& sol_decision_trace_action_is_authorized_v1(
					decision.sdt1, decision.sdt1_length, batch,
					batch_length, decision.sac1, decision.sac1_length))
			{
				seat->command = active_command;
				seat->decision = decision;
				seat->decision_ready = 1;
			}
			else
			{
				if (seat->brain_status >= SOL_BRAIN_NEUTRAL)
				{
					seat->brain_status = SOL_BRAIN_INTERNAL_ERROR;
				}
				seat->command = seat->neutral_command;
			}
		}
		if (results)
		{
			results[index].brain_status = seat->brain_status;
			results[index].action_length = seat->decision.sac1_length;
			results[index].trace_length = seat->decision.sdt1_length;
		}
	}
}

static int submit_all_bound_v1(sol_candidate_registry_v1 *registry,
	const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1],
	sol_candidate_frame_result_v1 *results)
{
	size_t index;
	int cancelled = 0;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_candidate_prepared_v1 *seat = &prepared[index];

		if (seat->bound_unhealthy)
		{
			cancelled = 1;
			continue;
		}
		if (!seat->participating || !seat->emittable)
		{
			continue;
		}
		if (seat->observation_lifecycle_invalid ||
			seat->observation_status == SOL_OBSERVATION_INVALID ||
			(seat->observation_status == SOL_OBSERVATION_READY &&
				!seat->decision_ready))
		{
			cancelled = 1;
		}
	}

	if (!cancelled)
	{
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			sol_candidate_entry_v1 *entry = &registry->entries[index];
			sol_candidate_prepared_v1 *seat = &prepared[index];

			if (!seat->participating || !seat->emittable ||
				seat->observation_status != SOL_OBSERVATION_READY ||
				!seat->decision_ready)
			{
				continue;
			}
			if (ops->decision(ops->context, index, entry->entity,
				entry->client_generation, seat->decision.sac1,
				seat->decision.sac1_length, seat->decision.sdt1,
				seat->decision.sdt1_length))
			{
				seat->decision_status = SOL_CANDIDATE_DECISION_SUBMITTED;
			}
			else
			{
				seat->decision_status = SOL_CANDIDATE_DECISION_REJECTED;
				cancelled = 1;
			}
		}
	}
	if (cancelled)
	{
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			sol_candidate_prepared_v1 *seat = &prepared[index];

			if (seat->participating && seat->emittable)
			{
				seat->command = seat->neutral_command;
			}
		}
	}
	if (results)
	{
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			results[index].decision_status = prepared[index].decision_status;
			results[index].frame_cancelled = cancelled &&
				((prepared[index].participating &&
					prepared[index].emittable) ||
					prepared[index].bound_unhealthy);
		}
	}
	return cancelled;
}

static size_t emit_all_bound_v1(sol_candidate_registry_v1 *registry,
	const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1],
	sol_candidate_frame_result_v1 *results, int cancelled)
{
	sol_candidate_command_batch_item_v1 items[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_command_batch_result_v1 batch_results[
		SOL_KTX_CANDIDATE_COUNT_V1];
	size_t item_indices[SOL_KTX_CANDIDATE_COUNT_V1];
	size_t emitted = 0;
	size_t count = 0;
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_candidate_entry_v1 *entry = &registry->entries[index];
		sol_candidate_prepared_v1 *seat = &prepared[index];

		if (!seat->participating || !seat->emittable)
		{
			continue;
		}
		items[count].index = index;
		items[count].entity = entry->entity;
		items[count].command = seat->command;
		items[count].neutral_command = seat->neutral_command;
		item_indices[count] = index;
		count++;
	}
	memset(batch_results, 0, sizeof(batch_results));
	if (count && !ops->commands(ops->context, items, count, batch_results))
	{
		cancelled = 1;
	}
	for (index = 0; index < count; ++index)
	{
		size_t candidate = item_indices[index];

		if (batch_results[index].request_status !=
				SOL_CANDIDATE_REQUEST_ACCEPTED || !batch_results[index].emitted)
		{
			cancelled = 1;
		}
		if (results)
		{
			results[candidate].request_status =
				batch_results[index].request_status;
			results[candidate].emitted = batch_results[index].emitted;
		}
		emitted += batch_results[index].emitted ? 1u : 0u;
	}
	if (cancelled && results)
	{
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			results[index].frame_cancelled =
				(prepared[index].participating && prepared[index].emittable) ||
				prepared[index].bound_unhealthy;
		}
	}
	return emitted;
}

size_t sol_candidate_registry_run_frame_v1(sol_candidate_registry_v1 *registry,
	uint8_t msec, uint32_t dt_us, const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1])
{
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1];
	size_t index;
	int cancelled;

	if (results)
	{
		memset(results, 0,
			SOL_KTX_CANDIDATE_COUNT_V1 * sizeof(results[0]));
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			results[index].index = index;
			results[index].observation_status = SOL_OBSERVATION_INVALID;
			results[index].brain_status = SOL_BRAIN_NEUTRAL;
			if (registry)
			{
				results[index].entity = registry->entries[index].entity;
				results[index].client_generation =
					registry->entries[index].client_generation;
			}
		}
	}
	if (!registry || !msec || !dt_us || !ops || !ops->healthy ||
		!ops->decision || !ops->commands)
	{
		return 0u;
	}
	memset(prepared, 0, sizeof(prepared));
	poll_all_bound_v1(registry, dt_us, ops, prepared, results);
	prepare_all_bound_v1(registry, msec, prepared, results);
	cancelled = submit_all_bound_v1(registry, ops, prepared, results);
	return emit_all_bound_v1(registry, ops, prepared, results, cancelled);
}
