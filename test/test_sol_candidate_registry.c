#include "sol_candidate_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct registry_fixture_v1 registry_fixture_v1;

typedef struct fake_observer_v1
{
	registry_fixture_v1 *fixture;
	size_t index;
	uint32_t slot;
	uint32_t generation;
	int profile_calls;
	int poll_calls;
	int invalid_poll;
} fake_observer_v1;

struct registry_fixture_v1
{
	sol_candidate_registry_v1 *registry;
	fake_observer_v1 observers[SOL_KTX_CANDIDATE_COUNT_V1];
	char trace[128];
	size_t trace_length;
	int command_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int remove_calls[SOL_KTX_CANDIDATE_COUNT_V1];
};

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void append_trace(registry_fixture_v1 *fixture, char phase, size_t index)
{
	require(fixture->trace_length + 2u < sizeof(fixture->trace),
			"phase trace remains bounded");
	fixture->trace[fixture->trace_length++] = phase;
	fixture->trace[fixture->trace_length++] = (char) ('1' + index);
	fixture->trace[fixture->trace_length] = '\0';
}

static intptr_t fake_observation_call(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	fake_observer_v1 *observer = context;

	if (operation == COV_GET_PROFILE)
	{
		static const uint8_t asset_id[32] = {
			0x57, 0x1c, 0x7e, 0x66, 0x68, 0xb2, 0xbc, 0x1f,
			0x25, 0x9d, 0x60, 0xf2, 0xfd, 0x32, 0x17, 0x11,
			0x3e, 0x5b, 0x06, 0x94, 0xf8, 0x54, 0x2c, 0xc2,
			0x89, 0x4a, 0x09, 0x84, 0xa8, 0x81, 0x2a, 0xb0
		};
		static const uint8_t sensory_id[32] = {
			0xb1, 0x5a, 0x18, 0xab, 0xfa, 0x89, 0xbc, 0x7e,
			0x53, 0xc8, 0xe9, 0x0b, 0xf7, 0x0f, 0xaf, 0x55,
			0x92, 0xee, 0x93, 0x47, 0xe1, 0x52, 0x8b, 0x7f,
			0xc1, 0x07, 0x7f, 0x12, 0xb2, 0x79, 0x2d, 0x92
		};
		cov_profile_v1 *profile = payload;

		require(payload_size == (intptr_t) sizeof(*profile),
				"profile request uses the exact ABI size");
		require(profile->engine_slot == observer->slot &&
			profile->client_generation == observer->generation &&
			!memcmp(profile->static_asset_set_id, asset_id, sizeof(asset_id)) &&
			!memcmp(profile->sensory_profile_id, sensory_id, sizeof(sensory_id)) &&
			profile->max_batch_bytes == COV_MAX_BATCH_BYTES_V1 &&
			profile->max_seen_entities == 96u &&
			profile->max_static_anchors == 16u &&
			profile->max_async_events == 128u,
				"private profile request is pre-sealed to the exact shared contract");
		observer->profile_calls++;
		return COV_RESULT_OK;
	}
	if (operation == COV_GET_COMMITTED)
	{
		cov_get_committed_v1 *get = payload;

		require(payload_size == (intptr_t) sizeof(*get),
				"committed request uses the exact ABI size");
		require(get->engine_slot == observer->slot &&
				get->client_generation == observer->generation,
				"every poll retains the seat-private generation route");
		observer->poll_calls++;
		append_trace(observer->fixture, 'P', observer->index);
		get->output_length = 0u;
		return observer->invalid_poll ? COV_RESULT_INVALID : COV_RESULT_EMPTY;
	}
	return COV_RESULT_INVALID;
}

static int seat_is_healthy(void *context, size_t index, int entity,
	uint32_t client_generation)
{
	registry_fixture_v1 *fixture = context;
	fake_observer_v1 *observer = &fixture->observers[index];

	return entity == (int) observer->slot &&
		client_generation == observer->generation;
}

static void write_command(void *context, size_t index, int entity,
	const sol_ktx_command_v1 *command)
{
	registry_fixture_v1 *fixture = context;
	unsigned poll_index;

	for (poll_index = 0; poll_index < SOL_KTX_CANDIDATE_COUNT_V1; ++poll_index)
	{
		if (!fixture->observers[poll_index].invalid_poll)
		{
			require(fixture->observers[poll_index].poll_calls == 1,
					"no command side effect occurs before all healthy polls");
		}
	}
	require(entity == (int) fixture->observers[index].slot,
			"command target is host-bound and seat-private");
	require(command->msec == 13u && command->angles[0] == 0.0f &&
		command->angles[1] == 0.0f && command->angles[2] == 0.0f &&
		command->forwardmove == 0 && command->sidemove == 0 &&
		command->upmove == 0 && command->buttons == 0u && command->impulse == 0u,
			"unadapted SOB1 input emits a complete fresh neutral command");
	fixture->command_calls[index]++;
	append_trace(fixture, 'C', index);
}

static void bind_four_private_candidates(sol_candidate_registry_v1 *registry,
	registry_fixture_v1 *fixture)
{
	static const uint32_t slots[SOL_KTX_CANDIDATE_COUNT_V1] = { 8u, 3u, 17u, 5u };
	static const uint32_t generations[SOL_KTX_CANDIDATE_COUNT_V1] = {
		101u, 202u, 303u, 404u
	};
	size_t index;

	fixture->registry = registry;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_ktx_seat_identity_v1 *identity =
				sol_ktx_plan_identity_v1((const char *[]) { "1", "2", "3", "4" }[index]);
		size_t claimed = SOL_KTX_CANDIDATE_COUNT_V1;

		fixture->observers[index].fixture = fixture;
		fixture->observers[index].index = index;
		fixture->observers[index].slot = slots[index];
		fixture->observers[index].generation = generations[index];
		require(sol_candidate_registry_set_pending_v1(registry, index),
				"empty candidate accepts one pending evidence seat");
		require(sol_candidate_registry_expect_client_v1(registry, index),
				"pending candidate enters one lifecycle claim window");
		if (index == 1u)
		{
			require(!sol_candidate_registry_claim_v1(registry,
					identity->player_name, (int) slots[0], &claimed),
					"a second seat cannot alias an entity already claimed by the first");
		}
		require(sol_candidate_registry_claim_v1(registry, identity->player_name,
				(int) slots[index], &claimed) && claimed == index,
				"expected bot name claims only its matching candidate entry");
		require(sol_candidate_registry_bind_v1(registry, index, generations[index],
				fake_observation_call, &fixture->observers[index]),
				"claimed candidate creates one private generation-bound observer");
		require(fixture->observers[index].profile_calls == 1,
				"each observer profile is queried exactly once at bind");
	}
}

static void remove_candidate(void *context, size_t index, int entity)
{
	registry_fixture_v1 *fixture = context;

	require(entity == (int) fixture->observers[index].slot,
			"termination removes the host-bound entity for this exact entry");
	fixture->remove_calls[index]++;
	append_trace(fixture, 'R', index);
	/* Production removal synchronously dispatches the disconnect hook. */
	require(sol_candidate_registry_release_v1(fixture->registry, index),
			"disconnect callback releases the same entry during removal");
}

static void test_four_candidate_lifecycle_has_no_cross_seat_alias(void)
{
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_entry_view_v1 entry;
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		size_t found = SOL_KTX_CANDIDATE_COUNT_V1;

		require(sol_candidate_registry_entry_v1(registry, index, &entry) &&
				entry.stage == SOL_CANDIDATE_BOUND && entry.index == index &&
				entry.identity->ordinal == index + 1u,
				"every candidate exposes its own bound lifecycle entry");
		require(sol_candidate_registry_find_entity_v1(registry, entry.entity, &found)
				&& found == index,
				"entity lookup resolves the owning entry without cross-seat alias");
	}
	require(sol_candidate_registry_release_v1(registry, 1u),
			"disconnect releases exactly one candidate entry");
	require(sol_candidate_registry_entry_v1(registry, 1u, &entry) &&
			entry.stage == SOL_CANDIDATE_EMPTY,
			"released candidate returns to empty");
	require(sol_candidate_registry_entry_v1(registry, 2u, &entry) &&
			entry.stage == SOL_CANDIDATE_BOUND && entry.client_generation == 303u,
			"disconnect cannot mutate another candidate generation or binding");
	sol_candidate_registry_destroy_v1(registry);
}

static void test_frame_polls_all_inputs_before_any_neutral_command(void)
{
	static const char expected_trace[] =
		"P1P2P3P4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, write_command
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "phase registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
			&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
			"one frame emits one command for all four healthy bound candidates");
	require(!strcmp(fixture.trace, expected_trace),
			"all four polls precede all command side effects");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(fixture.command_calls[index] == 1 && results[index].emitted,
				"every healthy candidate receives one command through the global writer");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_invalid_observer_emits_neutral_then_requires_termination(void)
{
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, write_command
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "failure registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	fixture.observers[1].invalid_poll = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
			&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
			"invalid observer still emits its one current-frame neutral pair");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(fixture.command_calls[index] == 1,
				"one invalid route cannot cancel any current-frame candidate writer");
	}
	require(results[1].observation_status == SOL_OBSERVATION_INVALID &&
			results[1].emitted,
			"invalid candidate is explicitly marked for post-frame termination");
	sol_candidate_registry_destroy_v1(registry);
}

static void test_termination_removes_every_candidate_without_double_release(void)
{
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_entry_view_v1 entry;
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "termination registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	require(sol_candidate_registry_remove_all_v1(registry, remove_candidate,
			&fixture) == SOL_KTX_CANDIDATE_COUNT_V1,
			"termination synchronously removes all four claimed candidate clients");
	require(!strcmp(fixture.trace, "R1R2R3R4"),
			"termination visits each candidate once in canonical seat order");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(fixture.remove_calls[index] == 1 &&
				sol_candidate_registry_entry_v1(registry, index, &entry) &&
				entry.stage == SOL_CANDIDATE_EMPTY,
				"synchronous disconnect leaves every registry entry empty once");
	}
	sol_candidate_registry_destroy_v1(registry);
}

int main(void)
{
	test_four_candidate_lifecycle_has_no_cross_seat_alias();
	test_frame_polls_all_inputs_before_any_neutral_command();
	test_invalid_observer_emits_neutral_then_requires_termination();
	test_termination_removes_every_candidate_without_double_release();
	printf("sol_candidate_registry: 4 contract tests passed\n");
	return 0;
}
