#include "sol_candidate_registry.h"
#include "sol_wire.h"

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
	int ready_poll;
} fake_observer_v1;

static const char observation_golden_hex[] =
	"534f42310000000000000000c8320000"
	"571c7e6668b2bc1f259d60f2fd3217113e5b0694f8542cc2894a0984a8812ab0"
	"115cfbcfeb3635f4cd7b29782ff4157abacd0c1e634f6b7b40a48e6064669732"
	"01000000000000000001010000000000000002000000"
	"0000000000000000000100"
	"0101020100f8ff10001800640038ff0000000000400080b000"
	"640000003200000007000000190000001e000000050000000c000000"
	"002000402000000011000304000d04"
	"0100000000000000010101"
	"00000000000000000000";

struct registry_fixture_v1
{
	sol_candidate_registry_v1 *registry;
	fake_observer_v1 observers[SOL_KTX_CANDIDATE_COUNT_V1];
	char trace[128];
	size_t trace_length;
	int command_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int request_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int decision_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int reject_decision[SOL_KTX_CANDIDATE_COUNT_V1];
	int reject_request[SOL_KTX_CANDIDATE_COUNT_V1];
	int unhealthy[SOL_KTX_CANDIDATE_COUNT_V1];
	int expect_quartet_neutral;
	int expect_no_decisions;
	int expect_zero_view;
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

static size_t hex_to_bytes(const char *hex, uint8_t *output, size_t capacity)
{
	size_t length = strlen(hex) / 2u;
	size_t index;

	require(strlen(hex) % 2u == 0u && length <= capacity,
			"observation fixture has bounded even hex length");
	for (index = 0; index < length; ++index)
	{
		unsigned value;

		require(sscanf(hex + (index * 2u), "%2x", &value) == 1,
				"observation fixture parses");
		output[index] = (uint8_t) value;
	}
	return length;
}

static void fixture_u32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t) value;
	output[1] = (uint8_t) (value >> 8);
	output[2] = (uint8_t) (value >> 16);
	output[3] = (uint8_t) (value >> 24);
}

static void fixture_u64(uint8_t *output, uint64_t value)
{
	fixture_u32(output, (uint32_t) value);
	fixture_u32(output + 4u, (uint32_t) (value >> 32));
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
			0x11, 0x5c, 0xfb, 0xcf, 0xeb, 0x36, 0x35, 0xf4,
			0xcd, 0x7b, 0x29, 0x78, 0x2f, 0xf4, 0x15, 0x7a,
			0xba, 0xcd, 0x0c, 0x1e, 0x63, 0x4f, 0x6b, 0x7b,
			0x40, 0xa4, 0x8e, 0x60, 0x64, 0x66, 0x97, 0x32
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
		if (observer->ready_poll)
		{
			size_t length = hex_to_bytes(observation_golden_hex, get->batch,
				get->output_capacity);

			fixture_u64(get->batch + 4u, get->frame_seq);
			fixture_u32(get->batch + 12u, get->dt_us);
			get->output_length = (uint32_t) length;
			return COV_RESULT_OK;
		}
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

	return !fixture->unhealthy[index] && entity == (int) observer->slot &&
		client_generation == observer->generation;
}

static int submit_decision(void *context, size_t index, int entity,
	uint32_t client_generation, const uint8_t *action_response,
	size_t action_response_length, const uint8_t *decision_trace,
	size_t decision_trace_length)
{
	registry_fixture_v1 *fixture = context;
	fake_observer_v1 *observer = &fixture->observers[index];
	sol_action_response_v1 decoded;
	unsigned poll_index;

	for (poll_index = 0; poll_index < SOL_KTX_CANDIDATE_COUNT_V1;
		++poll_index)
	{
		require(fixture->observers[poll_index].poll_calls == 1,
			"no decision evidence side effect occurs before all polls");
	}
	require(entity == (int) observer->slot &&
		client_generation == observer->generation,
		"decision evidence retains the seat-private binding route");
	require(sol_wire_decode_action_v1(action_response,
		action_response_length, &decoded) && decision_trace &&
		decision_trace_length > 0u &&
		!memcmp(decision_trace, "SDT1", 4u),
		"decision evidence carries complete canonical action and trace bytes");
	fixture->decision_calls[index]++;
	append_trace(fixture, 'D', index);
	return !fixture->reject_decision[index];
}

static void require_physical_command(registry_fixture_v1 *fixture, size_t index,
	int entity, const sol_ktx_command_v1 *command, int batch_cancelled)
{
	fake_observer_v1 *observer = &fixture->observers[index];
	int neutral = fixture->expect_quartet_neutral || batch_cancelled;

	require(entity == (int) fixture->observers[index].slot,
			"command target is host-bound and seat-private");
	if (observer->ready_poll)
	{
		require(command->msec == 13u && command->angles[0] == 0.0f &&
			command->angles[1] == (fixture->expect_zero_view ? 0.0f : 90.0f) &&
			command->angles[2] ==
				(fixture->expect_zero_view ? 0.0f : -180.0f) &&
			command->forwardmove == (neutral ? 0 : 400) &&
			command->sidemove == 0 &&
			command->upmove == 0 && command->buttons == 0u && command->impulse == 0u,
				"READY SOB1 emits either its proved action or fresh same-view neutral");
	}
	else
	{
		require(command->msec == 13u && command->angles[0] == 0.0f &&
			command->angles[1] == 0.0f && command->angles[2] == 0.0f &&
			command->forwardmove == 0 && command->sidemove == 0 &&
			command->upmove == 0 && command->buttons == 0u && command->impulse == 0u,
				"EMPTY or invalid input emits a complete fresh neutral command");
	}
}

static int write_commands(void *context,
	const sol_candidate_command_batch_item_v1 *items, size_t count,
	sol_candidate_command_batch_result_v1 *results)
{
	registry_fixture_v1 *fixture = context;
	sol_ktx_command_v1 physical[SOL_KTX_CANDIDATE_COUNT_V1];
	size_t item_index;
	unsigned poll_index;
	int cancelled = 0;

	for (poll_index = 0; poll_index < SOL_KTX_CANDIDATE_COUNT_V1; ++poll_index)
	{
		if (!fixture->unhealthy[poll_index])
		{
			require(fixture->observers[poll_index].poll_calls == 1,
					"no command request occurs before all healthy polls");
		}
		if (fixture->observers[poll_index].ready_poll &&
			!fixture->expect_no_decisions && !fixture->unhealthy[poll_index])
		{
			require(fixture->decision_calls[poll_index] == 1,
				"no command request occurs before all READY decisions submit");
		}
	}
	for (item_index = 0u; item_index < count; ++item_index)
	{
		size_t index = items[item_index].index;

		require(index < SOL_KTX_CANDIDATE_COUNT_V1 &&
			items[item_index].entity == (int) fixture->observers[index].slot,
				"batch request retains each host-bound seat identity");
		fixture->request_calls[index]++;
		append_trace(fixture, 'Q', index);
		results[item_index].request_status = fixture->reject_request[index] ?
			SOL_CANDIDATE_REQUEST_REJECTED : SOL_CANDIDATE_REQUEST_ACCEPTED;
		cancelled = cancelled || fixture->reject_request[index];
		physical[item_index] = items[item_index].command;
	}
	for (item_index = 0u; item_index < count; ++item_index)
	{
		size_t index = items[item_index].index;

		if (cancelled)
		{
			physical[item_index] = items[item_index].neutral_command;
		}
		require_physical_command(fixture, index, items[item_index].entity,
			&physical[item_index], cancelled);
		fixture->command_calls[index]++;
		results[item_index].emitted = 1;
		append_trace(fixture, 'C', index);
	}
	return !cancelled;
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
		if (index == 0u)
		{
			require(!sol_candidate_registry_bind_v1(registry, index,
					generations[index], 0u, fake_observation_call,
					&fixture->observers[index]),
				"candidate bind rejects an undeclared zero motion threshold");
		}
		require(sol_candidate_registry_bind_v1(registry, index, generations[index],
				SOL_BRAIN_STUCK_REPLAN_DEFAULT_MS_V1,
				fake_observation_call, &fixture->observers[index]),
				"claimed candidate creates one private generation-bound observer");
		require(fixture->observers[index].profile_calls == 1,
				"each observer profile is queried exactly once at bind");
	}
}

static void reset_frame_observations(registry_fixture_v1 *fixture)
{
	size_t index;

	fixture->trace_length = 0u;
	fixture->trace[0] = '\0';
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture->observers[index].poll_calls = 0;
		fixture->command_calls[index] = 0;
		fixture->request_calls[index] = 0;
		fixture->decision_calls[index] = 0;
	}
}

static void prime_initial_empty_frame(sol_candidate_registry_v1 *registry,
	registry_fixture_v1 *fixture, const sol_candidate_frame_ops_v1 *ops)
{
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	size_t index;

	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
		"first post-bind frame emits the sole allowed EMPTY neutral quartet");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(!results[index].frame_cancelled &&
			results[index].decision_status == SOL_CANDIDATE_DECISION_NOT_RUN,
			"initial EMPTY is explicit, proof-free, and nonfatal exactly once");
	}
	reset_frame_observations(fixture);
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
		"P1P2P3P4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
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
		require(fixture.request_calls[index] == 1 &&
			results[index].request_status == SOL_CANDIDATE_REQUEST_ACCEPTED &&
			fixture.command_calls[index] == 1 && results[index].emitted,
				"every healthy candidate receives one command through the global writer");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_invalid_observer_emits_neutral_then_requires_termination(void)
{
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "failure registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	fixture.observers[1].invalid_poll = 1;
	fixture.expect_quartet_neutral = 1;
	fixture.expect_no_decisions = 1;
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
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].frame_cancelled,
			"one invalid observation cancels active output for the whole quartet");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_only_first_post_bind_empty_frame_is_permitted(void)
{
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "EMPTY-cardinality registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	fixture.expect_quartet_neutral = 1;
	fixture.expect_no_decisions = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
		"second consecutive EMPTY still emits one fresh neutral writer quartet");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].observation_status == SOL_OBSERVATION_EMPTY &&
			results[index].frame_cancelled && results[index].emitted &&
			results[index].decision_status == SOL_CANDIDATE_DECISION_NOT_RUN,
			"second EMPTY is explicit quartet-fatal rather than indefinite warmup");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_ready_observations_drive_brains_after_global_poll_barrier(void)
{
	static const char expected_trace[] =
		"P1P2P3P4D1D2D3D4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "READY registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].ready_poll = 1;
	}
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
			&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
			"one READY frame emits one decided command for every candidate");
	require(!strcmp(fixture.trace, expected_trace),
			"every READY poll precedes every decided command side effect");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].observation_status == SOL_OBSERVATION_READY &&
			results[index].decision_status ==
				SOL_CANDIDATE_DECISION_SUBMITTED &&
			results[index].action_length == 33u &&
			results[index].trace_length > 0u && results[index].emitted &&
			fixture.decision_calls[index] == 1 &&
			fixture.command_calls[index] == 1,
				"each READY seat emits exactly one private-brain command");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_initial_ready_never_reaches_a_brain(void)
{
	static const char expected_trace[] =
		"P1P2P3P4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "initial-READY registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].ready_poll = 1;
	}
	fixture.expect_no_decisions = 1;
	fixture.expect_quartet_neutral = 1;
	fixture.expect_zero_view = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1 &&
		!strcmp(fixture.trace, expected_trace),
		"READY before the required initial EMPTY skips every brain and proof");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].observation_status == SOL_OBSERVATION_READY &&
			results[index].brain_status == SOL_BRAIN_NEUTRAL &&
			results[index].decision_status == SOL_CANDIDATE_DECISION_NOT_RUN &&
			results[index].frame_cancelled && results[index].emitted,
				"observation lifecycle failure stays outside the policy core");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_rejected_decision_neutralizes_the_complete_quartet(void)
{
	static const char expected_trace[] =
		"P1P2P3P4D1D2D3D4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "rejected-decision registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].ready_poll = 1;
	}
	fixture.reject_decision[1] = 1;
	fixture.expect_quartet_neutral = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1 &&
		!strcmp(fixture.trace, expected_trace),
		"all decision submissions finish before rejected-seat command fallback");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(fixture.decision_calls[index] == 1 &&
			fixture.command_calls[index] == 1 && results[index].emitted,
			"decision rejection cannot cancel another seat's writer cadence");
		if (index == 1u)
		{
			require(results[index].decision_status ==
					SOL_CANDIDATE_DECISION_REJECTED &&
				results[index].brain_status == SOL_BRAIN_DECISION,
				"rejected proof is explicit without misattributing brain failure");
		}
		else
		{
			require(results[index].decision_status ==
					SOL_CANDIDATE_DECISION_SUBMITTED &&
				results[index].brain_status == SOL_BRAIN_DECISION,
				"proofs accepted before quartet cancellation retain truthful status");
		}
		require(results[index].frame_cancelled,
			"one proof rejection cancels the complete quartet frame");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_request_rejection_neutralizes_before_any_physical_emit(void)
{
	static const char expected_trace[] =
		"P1P2P3P4D1D2D3D4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "request-rejection registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].ready_poll = 1;
	}
	fixture.reject_request[1] = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1 &&
		!strcmp(fixture.trace, expected_trace),
		"request-all barrier completes before a rejected quartet emits neutral");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].decision_status ==
				SOL_CANDIDATE_DECISION_SUBMITTED &&
			results[index].request_status == (index == 1u ?
				SOL_CANDIDATE_REQUEST_REJECTED :
				SOL_CANDIDATE_REQUEST_ACCEPTED) &&
			results[index].frame_cancelled && results[index].emitted &&
			fixture.request_calls[index] == 1 &&
			fixture.command_calls[index] == 1,
				"request rejection is truthful and physically neutral for every seat");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_rebind_gets_a_fresh_initial_empty_lifecycle(void)
{
	static const char expected_trace[] =
		"P1P2P3P4Q1Q2Q3Q4C1C2C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "rebind registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(sol_candidate_registry_unbind_v1(registry, index) &&
			sol_candidate_registry_bind_v1(registry, index,
				fixture.observers[index].generation,
				SOL_BRAIN_STUCK_REPLAN_DEFAULT_MS_V1, fake_observation_call,
				&fixture.observers[index]) &&
			fixture.observers[index].profile_calls == 2,
				"each observer can be safely destroyed and rebound to the same client");
	}
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1 &&
		!strcmp(fixture.trace, expected_trace),
		"fresh observers receive a fresh first EMPTY frame after rebind");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(!results[index].frame_cancelled && results[index].emitted &&
			results[index].request_status == SOL_CANDIDATE_REQUEST_ACCEPTED,
				"rebind does not inherit the destroyed observer lifecycle latch");
	}
	sol_candidate_registry_destroy_v1(registry);
}

static void test_bound_unhealthy_seat_cancels_live_fire_for_quartet(void)
{
	static const char expected_trace[] = "P1P3P4Q1Q3Q4C1C3C4";
	registry_fixture_v1 fixture = { 0 };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_candidate_frame_ops_v1 ops = {
		&fixture, seat_is_healthy, submit_decision, write_commands
	};
	sol_candidate_registry_v1 *registry = sol_candidate_registry_create_v1();
	size_t index;

	require(registry != NULL, "unhealthy-seat registry allocation succeeds");
	bind_four_private_candidates(registry, &fixture);
	prime_initial_empty_frame(registry, &fixture, &ops);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].ready_poll = 1;
	}
	fixture.unhealthy[1] = 1;
	fixture.expect_quartet_neutral = 1;
	fixture.expect_no_decisions = 1;
	require(sol_candidate_registry_run_frame_v1(registry, 13u, 13000u,
		&ops, results) == SOL_KTX_CANDIDATE_COUNT_V1 - 1u &&
		!strcmp(fixture.trace, expected_trace),
		"BOUND unhealthy seat suppresses every proof and active peer command");
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(results[index].frame_cancelled,
			"unhealthy binding marks the whole quartet frame cancelled");
		if (index == 1u)
		{
			require(!results[index].emitted &&
				fixture.command_calls[index] == 0,
				"unhealthy target is not called through an unsafe writer route");
		}
		else
		{
			require(results[index].emitted &&
				fixture.command_calls[index] == 1 &&
				results[index].decision_status ==
					SOL_CANDIDATE_DECISION_NOT_RUN,
				"healthy peers emit only fresh neutral after quartet cancellation");
		}
	}
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
	test_only_first_post_bind_empty_frame_is_permitted();
	test_ready_observations_drive_brains_after_global_poll_barrier();
	test_initial_ready_never_reaches_a_brain();
	test_rejected_decision_neutralizes_the_complete_quartet();
	test_request_rejection_neutralizes_before_any_physical_emit();
	test_rebind_gets_a_fresh_initial_empty_lifecycle();
	test_bound_unhealthy_seat_cancels_live_fire_for_quartet();
	test_termination_removes_every_candidate_without_double_release();
	printf("sol_candidate_registry: 11 contract tests passed\n");
	return 0;
}
