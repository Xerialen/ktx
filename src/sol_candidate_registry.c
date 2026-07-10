#include "sol_candidate_registry.h"

#include <stdlib.h>
#include <string.h>

#define SOL_CANDIDATE_MAX_ENGINE_SLOT_V1 32

typedef struct sol_candidate_entry_v1
{
	sol_candidate_stage_v1 stage;
	int entity;
	uint32_t client_generation;
	sol_observation_client_v1 *observation;
} sol_candidate_entry_v1;

typedef struct sol_candidate_prepared_v1
{
	int participating;
	int emittable;
	sol_observation_status_v1 observation_status;
	size_t batch_length;
	sol_ktx_command_v1 command;
	uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE];
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
	size_t index, uint32_t client_generation, sol_observation_call_v1 call,
	void *call_context)
{
	sol_candidate_entry_v1 *entry;
	sol_observation_client_v1 *observation;

	if (!registry || !valid_index(index) || !client_generation || !call)
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
	entry->client_generation = client_generation;
	entry->observation = observation;
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
	if (entry->stage != SOL_CANDIDATE_BOUND || !entry->observation ||
		!entry->client_generation)
	{
		return 0;
	}
	sol_observation_client_destroy_v1(entry->observation);
	entry->observation = NULL;
	entry->client_generation = 0u;
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

		if (entry->stage != SOL_CANDIDATE_BOUND || !entry->observation ||
			!ops->healthy(ops->context, index, entry->entity,
				entry->client_generation))
		{
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
		if (results)
		{
			results[index].observation_status = seat->observation_status;
			results[index].batch_length = seat->batch_length;
		}
	}
}

static void prepare_all_bound_v1(sol_candidate_registry_v1 *registry,
	uint8_t msec,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1])
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
		neutral_command(msec, entry->observation, &seat->command);
		if (!sol_ktx_encode_command_v1(&seat->command, seat->command_wire))
		{
			neutral_command(msec, NULL, &seat->command);
			seat->emittable = sol_ktx_encode_command_v1(&seat->command,
				seat->command_wire);
		}
	}
}

static size_t emit_all_bound_v1(sol_candidate_registry_v1 *registry,
	const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1],
	sol_candidate_frame_result_v1 *results)
{
	size_t emitted = 0;
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_candidate_entry_v1 *entry = &registry->entries[index];
		sol_candidate_prepared_v1 *seat = &prepared[index];
		int evidence_result;

		if (!seat->participating || !seat->emittable)
		{
			continue;
		}
		evidence_result = ops->evidence(ops->context, index, entry->entity,
			entry->client_generation, seat->command_wire);
		ops->command(ops->context, index, entry->entity, &seat->command);
		if (results)
		{
			results[index].evidence_result = evidence_result;
			results[index].emitted = 1;
		}
		emitted++;
	}
	return emitted;
}

size_t sol_candidate_registry_run_frame_v1(sol_candidate_registry_v1 *registry,
	uint8_t msec, uint32_t dt_us, const sol_candidate_frame_ops_v1 *ops,
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1])
{
	sol_candidate_prepared_v1 prepared[SOL_KTX_CANDIDATE_COUNT_V1];
	size_t index;

	if (results)
	{
		memset(results, 0,
			SOL_KTX_CANDIDATE_COUNT_V1 * sizeof(results[0]));
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			results[index].index = index;
			results[index].observation_status = SOL_OBSERVATION_INVALID;
			if (registry)
			{
				results[index].entity = registry->entries[index].entity;
				results[index].client_generation =
					registry->entries[index].client_generation;
			}
		}
	}
	if (!registry || !msec || !dt_us || !ops || !ops->healthy ||
		!ops->evidence || !ops->command)
	{
		return 0u;
	}
	memset(prepared, 0, sizeof(prepared));
	poll_all_bound_v1(registry, dt_us, ops, prepared, results);
	prepare_all_bound_v1(registry, msec, prepared);
	return emit_all_bound_v1(registry, ops, prepared, results);
}
