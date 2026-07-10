#include "sol_candidate_registry.h"
#include "sol_evidence_run.h"
#include "sol_runtime_schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum fake_epoch_phase_v1
{
	FAKE_EPOCH_IDLE = 0,
	FAKE_EPOCH_OPEN,
	FAKE_EPOCH_ENDED
} fake_epoch_phase_v1;

typedef struct timing_fixture_v1 timing_fixture_v1;

typedef struct fake_observer_v1
{
	size_t index;
	uint32_t slot;
	uint32_t generation;
	int poll_calls;
} fake_observer_v1;

typedef struct fake_binding_v1
{
	int active;
	uint32_t slot;
	uint32_t generation;
} fake_binding_v1;

struct timing_fixture_v1
{
	sol_evidence_run_v1 *run;
	sol_candidate_registry_v1 *registry;
	fake_observer_v1 observers[SOL_KTX_CANDIDATE_COUNT_V1];
	fake_binding_v1 bindings[SOL_KTX_CANDIDATE_COUNT_V1];
	fake_epoch_phase_v1 epoch_phase;
	int bot_phase;
	int begin_calls;
	int end_calls;
	int unbind_calls;
	int remove_calls;
	int request_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int command_calls[SOL_KTX_CANDIDATE_COUNT_V1];
	int reject_unbind_once[SOL_KTX_CANDIDATE_COUNT_V1];
	char cleanup_trace[64];
	size_t cleanup_trace_length;
};

static const uint32_t slots[SOL_KTX_CANDIDATE_COUNT_V1] = {
	7u, 8u, 9u, 10u
};
static const uint32_t generations[SOL_KTX_CANDIDATE_COUNT_V1] = {
	101u, 202u, 303u, 404u
};
static const char run_nonce[] =
	"1111111111111111111111111111111111111111111111111111111111111111";
static const char second_run_nonce[] =
	"2222222222222222222222222222222222222222222222222222222222222222";
static const char seat_nonces[SOL_KTX_CANDIDATE_COUNT_V1][CE_SEAT_NONCE_CAP] = {
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
	"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
};

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void append_cleanup_trace(timing_fixture_v1 *fixture, char operation,
	size_t index)
{
	require(fixture->cleanup_trace_length + 2u < sizeof(fixture->cleanup_trace),
			"cleanup trace remains bounded");
	fixture->cleanup_trace[fixture->cleanup_trace_length++] = operation;
	if (index < SOL_KTX_CANDIDATE_COUNT_V1)
	{
		fixture->cleanup_trace[fixture->cleanup_trace_length++] =
			(char) ('1' + index);
	}
	fixture->cleanup_trace[fixture->cleanup_trace_length] = '\0';
}

static int any_binding(const timing_fixture_v1 *fixture)
{
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		if (fixture->bindings[index].active)
		{
			return 1;
		}
	}
	return 0;
}

static intptr_t fake_evidence_call(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	timing_fixture_v1 *fixture = context;

	if (operation == CE_MATCH_BEGIN)
	{
		ce_epoch_begin_v1 *begin = payload;

		require(!fixture->bot_phase && fixture->epoch_phase == FAKE_EPOCH_IDLE
			&& payload_size == (intptr_t) sizeof(*begin),
				"MATCH_BEGIN is server-only and starts from idle");
		fixture->epoch_phase = FAKE_EPOCH_OPEN;
		fixture->begin_calls++;
		return CE_RESULT_OK;
	}
	if (operation == CE_MATCH_END)
	{
		ce_epoch_end_v1 *end = payload;

		require(!fixture->bot_phase && fixture->epoch_phase == FAKE_EPOCH_OPEN
			&& payload_size == (intptr_t) sizeof(*end),
				"MATCH_END is deferred to an eventual server callback");
		fixture->epoch_phase = any_binding(fixture) ?
			FAKE_EPOCH_ENDED : FAKE_EPOCH_IDLE;
		fixture->end_calls++;
		append_cleanup_trace(fixture, 'E', SOL_KTX_CANDIDATE_COUNT_V1);
		return CE_RESULT_OK;
	}
	if (operation == CE_UNBIND)
	{
		ce_unbind_v1 *unbind = payload;
		size_t index;

		require(!fixture->bot_phase && fixture->epoch_phase == FAKE_EPOCH_ENDED
			&& payload_size == (intptr_t) sizeof(*unbind),
				"UNBIND is server-only and follows MATCH_END");
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			if (fixture->bindings[index].active
				&& fixture->bindings[index].slot == unbind->engine_slot
				&& fixture->bindings[index].generation ==
					unbind->client_generation)
			{
				if (fixture->reject_unbind_once[index])
				{
					fixture->reject_unbind_once[index] = 0;
					append_cleanup_trace(fixture, 'X', index);
					return CE_RESULT_INVALID;
				}
				fixture->bindings[index].active = 0;
				fixture->unbind_calls++;
				append_cleanup_trace(fixture, 'U', index);
				if (!any_binding(fixture))
				{
					fixture->epoch_phase = FAKE_EPOCH_IDLE;
				}
				return CE_RESULT_OK;
			}
		}
		require(0, "UNBIND retains an exact successful CE route");
	}
	return CE_RESULT_INVALID;
}

static intptr_t fake_observation_call(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	fake_observer_v1 *observer = context;

	if (operation == COV_GET_PROFILE)
	{
		cov_profile_v1 *profile = payload;

		require(payload_size == (intptr_t) sizeof(*profile)
			&& profile->engine_slot == observer->slot
			&& profile->client_generation == observer->generation,
				"observer profile remains seat-private");
		memset(profile->static_asset_set_id, (int) (0x10u + observer->index), 32u);
		memset(profile->sensory_profile_id, (int) (0x20u + observer->index), 32u);
		profile->max_batch_bytes = COV_MAX_BATCH_BYTES_V1;
		profile->max_seen_entities = 96u;
		profile->max_static_anchors = 16u;
		profile->max_async_events = 128u;
		return COV_RESULT_OK;
	}
	if (operation == COV_GET_COMMITTED)
	{
		cov_get_committed_v1 *get = payload;

		require(payload_size == (intptr_t) sizeof(*get)
			&& get->engine_slot == observer->slot
			&& get->client_generation == observer->generation,
				"pending-close poll keeps its private generation route");
		observer->poll_calls++;
		get->output_length = 0u;
		return COV_RESULT_EMPTY;
	}
	return COV_RESULT_INVALID;
}

static int candidate_healthy(void *context, size_t index, int entity,
	uint32_t client_generation)
{
	timing_fixture_v1 *fixture = context;

	return entity == (int) fixture->observers[index].slot
		&& client_generation == fixture->observers[index].generation;
}

static int write_complete_request(void *context, size_t index, int entity,
	uint32_t client_generation,
	const uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE])
{
	timing_fixture_v1 *fixture = context;
	size_t candidate;

	require(fixture->bot_phase, "CE frame requests occur only in bot callbacks");
	for (candidate = 0; candidate < SOL_KTX_CANDIDATE_COUNT_V1; ++candidate)
	{
		require(fixture->observers[candidate].poll_calls == 1,
				"every live seat is polled before any request is emitted");
	}
	require(entity == (int) slots[index]
		&& client_generation == generations[index]
		&& !memcmp(command_wire, "SUC1", 4u),
			"each request carries one complete canonical neutral command");
	fixture->request_calls[index]++;
	return CE_RESULT_OK;
}

static void write_actual_command(void *context, size_t index, int entity,
	const sol_ktx_command_v1 *command)
{
	timing_fixture_v1 *fixture = context;

	require(fixture->bot_phase && entity == (int) slots[index]
		&& fixture->request_calls[index] == fixture->command_calls[index] + 1,
			"actual command follows its complete CE request in the same callback");
	require(command->msec == 13u && command->angles[0] == 0.0f
		&& command->angles[1] == 0.0f && command->angles[2] == 0.0f
		&& command->forwardmove == 0 && command->sidemove == 0
		&& command->upmove == 0 && command->buttons == 0u
		&& command->impulse == 0u,
			"pending-close callback emits a fresh deterministic neutral command");
	fixture->command_calls[index]++;
}

static void bind_four_live_candidates(timing_fixture_v1 *fixture)
{
	size_t index;

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		const sol_ktx_seat_identity_v1 *identity = sol_ktx_plan_identity_v1(
			(const char *[]) { "1", "2", "3", "4" }[index]);
		size_t claimed = SOL_KTX_CANDIDATE_COUNT_V1;

		fixture->observers[index].index = index;
		fixture->observers[index].slot = slots[index];
		fixture->observers[index].generation = generations[index];
		require(sol_evidence_run_configure_seat_v1(fixture->run, index,
				seat_nonces[index])
			&& sol_evidence_run_record_client_v1(fixture->run, index, slots[index]),
				"evidence lifecycle records each live client route");
		fixture->bindings[index].active = 1;
		fixture->bindings[index].slot = slots[index];
		fixture->bindings[index].generation = generations[index];
		require(sol_evidence_run_record_bind_v1(fixture->run, index, slots[index],
				generations[index]),
				"evidence lifecycle records each successful CE route");
		require(sol_candidate_registry_set_pending_v1(fixture->registry, index)
			&& sol_candidate_registry_expect_client_v1(fixture->registry, index)
			&& sol_candidate_registry_claim_v1(fixture->registry,
				identity->player_name, (int) slots[index], &claimed)
			&& claimed == index
			&& sol_candidate_registry_bind_v1(fixture->registry, index,
				generations[index], fake_observation_call,
				&fixture->observers[index]),
				"runtime registry binds four independent live candidate seats");
	}
}

static void remove_live_candidate(void *context, size_t index,
	uint32_t engine_slot)
{
	timing_fixture_v1 *fixture = context;

	require(!fixture->bot_phase && engine_slot == slots[index],
			"client removal is deferred and retains its original route");
	fixture->remove_calls++;
	append_cleanup_trace(fixture, 'R', index);
	require(sol_evidence_run_note_disconnect_v1(fixture->run, index)
		&& sol_candidate_registry_release_v1(fixture->registry, index),
			"synchronous removal dispatches both disconnect owners exactly once");
}

static void test_pending_cleanup_keeps_complete_bot_frames_until_server_hook(void)
{
	timing_fixture_v1 fixture = { 0 };
	sol_candidate_frame_ops_v1 frame_ops = {
		&fixture, candidate_healthy, write_complete_request, write_actual_command
	};
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_runtime_schedule_decision_v1 schedule;
	size_t frame;
	size_t index;

	fixture.run = sol_evidence_run_create_v1();
	fixture.registry = sol_candidate_registry_create_v1();
	require(fixture.run != NULL && fixture.registry != NULL
		&& sol_evidence_run_begin_v1(fixture.run, run_nonce, "ktx-match/v1",
			fake_evidence_call, &fixture), "timing fixture begins one evidence epoch");
	bind_four_live_candidates(&fixture);
	fixture.cleanup_trace[0] = '\0';
	sol_evidence_run_fail_stop_v1(fixture.run);
	require(sol_evidence_run_cleanup_pending_v1(fixture.run),
			"fail-stop marks cleanup pending before the adversarial bot-only interval");

	for (frame = 0; frame < 2u; ++frame)
	{
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			fixture.observers[index].poll_calls = 0;
		}
		schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_BOT_FRAME_V1,
			sol_evidence_run_active_v1(fixture.run),
			sol_evidence_run_emissions_open_v1(fixture.run), fixture.registry != NULL,
			sol_evidence_run_cleanup_pending_v1(fixture.run));
		require(schedule.run_candidates && !schedule.run_cleanup,
				"cleanup-pending bot callback still schedules all live candidates");
		fixture.bot_phase = 1;
		require(sol_candidate_registry_run_frame_v1(fixture.registry, 13u, 13000u,
				&frame_ops, results) == SOL_KTX_CANDIDATE_COUNT_V1,
				"each intervening bot callback emits all four complete writer pairs");
		fixture.bot_phase = 0;
		for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
		{
			require(results[index].emitted
				&& fixture.request_calls[index] == (int) frame + 1
				&& fixture.command_calls[index] == (int) frame + 1,
					"every live seat receives exactly one request and command per callback");
		}
		require(fixture.end_calls == 0 && fixture.unbind_calls == 0
			&& fixture.remove_calls == 0,
				"bot callbacks perform no END, UNBIND, or removal side effect");
	}

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_SERVER_FRAME_V1,
		sol_evidence_run_active_v1(fixture.run),
		sol_evidence_run_emissions_open_v1(fixture.run), fixture.registry != NULL,
		sol_evidence_run_cleanup_pending_v1(fixture.run));
	require(!schedule.run_candidates && schedule.run_cleanup,
			"eventual non-bot callback exclusively schedules safe cleanup");
	fixture.reject_unbind_once[2] = 1;
	require(sol_evidence_run_server_cleanup_v1(fixture.run,
			remove_live_candidate, &fixture) == SOL_EVIDENCE_CLEANUP_RETRY
		&& !strcmp(fixture.cleanup_trace, "EU1U2X3U4")
		&& fixture.end_calls == 1 && fixture.unbind_calls == 3
		&& fixture.remove_calls == 0,
			"first server callback ends once but retains a rejected UNBIND route");

	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		fixture.observers[index].poll_calls = 0;
	}
	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_BOT_FRAME_V1,
		sol_evidence_run_active_v1(fixture.run),
		sol_evidence_run_emissions_open_v1(fixture.run), fixture.registry != NULL,
		sol_evidence_run_cleanup_pending_v1(fixture.run));
	fixture.bot_phase = 1;
	if (schedule.run_candidates)
	{
		(void) sol_candidate_registry_run_frame_v1(fixture.registry, 13u, 13000u,
			&frame_ops, results);
	}
	fixture.bot_phase = 0;
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		require(fixture.observers[index].poll_calls == 0
			&& fixture.request_calls[index] == 2
			&& fixture.command_calls[index] == 2,
				"bot callback after END attempt is fully inert during UNBIND retry");
	}
	require(fixture.end_calls == 1 && fixture.unbind_calls == 3
		&& fixture.remove_calls == 0
		&& !strcmp(fixture.cleanup_trace, "EU1U2X3U4"),
			"intervening inert bot callback performs no cleanup operation");

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_SERVER_FRAME_V1,
		sol_evidence_run_active_v1(fixture.run),
		sol_evidence_run_emissions_open_v1(fixture.run), fixture.registry != NULL,
		sol_evidence_run_cleanup_pending_v1(fixture.run));
	require(!schedule.run_candidates && schedule.run_cleanup
		&& sol_evidence_run_server_cleanup_v1(fixture.run,
			remove_live_candidate, &fixture) == SOL_EVIDENCE_CLEANUP_COMPLETE
		&& !strcmp(fixture.cleanup_trace, "EU1U2X3U4U3R1R2R3R4")
		&& fixture.end_calls == 1 && fixture.unbind_calls == 4
		&& fixture.remove_calls == 4,
			"later server callback retries only UNBIND then removes and completes");
	require(sol_evidence_run_begin_v1(fixture.run, second_run_nonce,
			"ktx-match/v1", fake_evidence_call, &fixture)
		&& fixture.begin_calls == 2,
			"successful eventual cleanup permits a reusable second epoch");
	sol_evidence_run_request_close_v1(fixture.run);
	require(sol_evidence_run_server_cleanup_v1(fixture.run,
			remove_live_candidate, &fixture) == SOL_EVIDENCE_CLEANUP_COMPLETE,
			"empty second epoch closes without retaining timing state");
	sol_candidate_registry_destroy_v1(fixture.registry);
	sol_evidence_run_destroy_v1(fixture.run);
}

int main(void)
{
	test_pending_cleanup_keeps_complete_bot_frames_until_server_hook();
	printf("sol_runtime_schedule: adversarial timing contract passed\n");
	return 0;
}
