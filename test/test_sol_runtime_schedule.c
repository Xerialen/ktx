#include "sol_actual_command.h"
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
	int profile_calls;
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
	fake_binding_v1 bindings[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
	fake_epoch_phase_v1 epoch_phase;
	int bot_phase;
	int begin_calls;
	int end_calls;
	int unbind_calls;
	int remove_calls;
	int request_calls[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
	int command_calls[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
	int reject_unbind_once[SOL_KTX_EVIDENCE_SEAT_COUNT_V1];
	char cleanup_trace[128];
	size_t cleanup_trace_length;
};

static const uint32_t slots[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
	7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u
};
static const uint32_t generations[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
	101u, 202u, 303u, 404u, 505u, 606u, 707u, 808u
};
static const char run_nonce[] =
	"1111111111111111111111111111111111111111111111111111111111111111";
static const char second_run_nonce[] =
	"2222222222222222222222222222222222222222222222222222222222222222";
static const char seat_nonces[SOL_KTX_EVIDENCE_SEAT_COUNT_V1][CE_SEAT_NONCE_CAP] = {
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
	"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
	"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
	"1111111111111111111111111111111111111111111111111111111111111111",
	"2222222222222222222222222222222222222222222222222222222222222222"
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
	if (index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1)
	{
		fixture->cleanup_trace[fixture->cleanup_trace_length++] =
			(char) ('1' + index);
	}
	fixture->cleanup_trace[fixture->cleanup_trace_length] = '\0';
}

static int any_binding(const timing_fixture_v1 *fixture)
{
	size_t index;

	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
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
		append_cleanup_trace(fixture, 'E', SOL_KTX_EVIDENCE_SEAT_COUNT_V1);
		return CE_RESULT_OK;
	}
	if (operation == CE_UNBIND)
	{
		ce_unbind_v1 *unbind = payload;
		size_t index;

		require(!fixture->bot_phase && fixture->epoch_phase == FAKE_EPOCH_ENDED
			&& payload_size == (intptr_t) sizeof(*unbind),
				"UNBIND is server-only and follows MATCH_END");
		for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
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

		require(payload_size == (intptr_t) sizeof(*profile)
			&& profile->engine_slot == observer->slot
			&& profile->client_generation == observer->generation
			&& !memcmp(profile->static_asset_set_id, asset_id, sizeof(asset_id))
			&& !memcmp(profile->sensory_profile_id, sensory_id, sizeof(sensory_id))
			&& profile->max_batch_bytes == COV_MAX_BATCH_BYTES_V1
			&& profile->max_seen_entities == 96u
			&& profile->max_static_anchors == 16u
			&& profile->max_async_events == 128u,
				"candidate profile is pre-sealed and seat-private");
		observer->profile_calls++;
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

static size_t index_for_slot(uint32_t slot)
{
	size_t index;

	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
	{
		if (slots[index] == slot)
		{
			return index;
		}
	}
	return SOL_KTX_EVIDENCE_SEAT_COUNT_V1;
}

static sol_actual_command_route_v1 lookup_command(void *context,
	uint32_t engine_slot, uint32_t *client_generation)
{
	timing_fixture_v1 *fixture = context;
	sol_evidence_binding_lookup_result_v1 result;

	if (!sol_evidence_run_emissions_open_v1(fixture->run))
	{
		return SOL_ACTUAL_COMMAND_UNBOUND;
	}
	result = sol_evidence_run_find_binding_v1(fixture->run, engine_slot, NULL,
		client_generation);
	return result == SOL_EVIDENCE_BINDING_EXACT ? SOL_ACTUAL_COMMAND_BOUND :
		result == SOL_EVIDENCE_BINDING_AMBIGUOUS ? SOL_ACTUAL_COMMAND_AMBIGUOUS :
		SOL_ACTUAL_COMMAND_UNBOUND;
}

static intptr_t write_complete_request(void *context,
	const ce_frame_request_v1 *request)
{
	timing_fixture_v1 *fixture = context;
	size_t candidate;
	size_t index = index_for_slot(request->engine_slot);

	require(fixture->bot_phase, "CE frame requests occur only in bot callbacks");
	for (candidate = 0; candidate < SOL_KTX_CANDIDATE_COUNT_V1; ++candidate)
	{
		require(fixture->observers[candidate].poll_calls == 1,
				"every live seat is polled before any request is emitted");
	}
	require(index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		&& request->client_generation == generations[index]
		&& !memcmp(request->requested_command.bytes, "SUC1", 4u),
			"each request carries one complete canonical neutral command");
	fixture->request_calls[index]++;
	return CE_RESULT_OK;
}

static intptr_t write_actual_command(void *context,
	const sol_actual_command_input_v1 *command)
{
	timing_fixture_v1 *fixture = context;
	size_t index = index_for_slot((uint32_t) command->engine_slot);

	require(fixture->bot_phase && index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1
		&& fixture->request_calls[index] == fixture->command_calls[index] + 1,
			"actual command follows its complete CE request in the same callback");
	require(command->msec == 13 && command->angles[0] == 0.0f
		&& command->angles[1] == 0.0f && command->angles[2] == 0.0f
		&& command->forwardmove == 0 && command->sidemove == 0
		&& command->upmove == 0 && command->buttons == 0u
		&& command->impulse == 0u,
			"pending-close callback emits a fresh deterministic neutral command");
	fixture->command_calls[index]++;
	return 1;
}

static void unexpected_fail_stop(void *context)
{
	(void) context;
	require(0, "valid eight-seat command fixture never fail-stops");
}

static void submit_command(timing_fixture_v1 *fixture, size_t index,
	const sol_ktx_command_v1 *command)
{
	sol_actual_command_input_v1 input;
	sol_actual_command_ops_v1 ops = {
		fixture, lookup_command, write_complete_request, write_actual_command,
		unexpected_fail_stop
	};

	memset(&input, 0, sizeof(input));
	input.engine_slot = (intptr_t) slots[index];
	input.msec = command->msec;
	memcpy(input.angles, command->angles, sizeof(input.angles));
	input.forwardmove = command->forwardmove;
	input.sidemove = command->sidemove;
	input.upmove = command->upmove;
	input.buttons = command->buttons;
	input.impulse = command->impulse;
	require(sol_actual_command_submit_v1(&input, &ops) == 1,
			"global command hook submits one exact actual command");
}

static void write_candidate_command(void *context, size_t index, int entity,
	const sol_ktx_command_v1 *command)
{
	timing_fixture_v1 *fixture = context;

	require(entity == (int) slots[index],
			"candidate registry targets its exact bound engine slot");
	submit_command(fixture, index, command);
}

static void bind_eight_live_seats(timing_fixture_v1 *fixture)
{
	size_t index;

	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
	{
		require(sol_evidence_run_configure_seat_v1(fixture->run, index,
				seat_nonces[index])
			&& sol_evidence_run_record_client_v1(fixture->run, index, slots[index]),
				"evidence lifecycle records each live client route");
		fixture->bindings[index].active = 1;
		fixture->bindings[index].slot = slots[index];
		fixture->bindings[index].generation = generations[index];
		require(sol_evidence_run_record_bind_v1(fixture->run, index, slots[index],
				generations[index]) == SOL_EVIDENCE_BIND_ACCEPTED,
				"evidence lifecycle records each successful CE route");
		if (index < SOL_KTX_CANDIDATE_COUNT_V1)
		{
			const sol_ktx_seat_identity_v1 *identity = sol_ktx_plan_identity_v1(
				(const char *[]) { "1", "2", "3", "4" }[index]);
			size_t claimed = SOL_KTX_CANDIDATE_COUNT_V1;

			fixture->observers[index].index = index;
			fixture->observers[index].slot = slots[index];
			fixture->observers[index].generation = generations[index];
			require(sol_candidate_registry_set_pending_v1(fixture->registry, index)
				&& sol_candidate_registry_expect_client_v1(fixture->registry, index)
				&& sol_candidate_registry_claim_v1(fixture->registry,
					identity->player_name, (int) slots[index], &claimed)
				&& claimed == index
				&& sol_candidate_registry_bind_v1(fixture->registry, index,
					generations[index], fake_observation_call,
					&fixture->observers[index])
				&& fixture->observers[index].profile_calls == 1,
					"exactly four candidates bind one profile; controls bind none");
		}
	}
}

static void remove_live_seat(void *context, size_t index,
	uint32_t engine_slot)
{
	timing_fixture_v1 *fixture = context;

	require(!fixture->bot_phase && engine_slot == slots[index],
			"client removal is deferred and retains its original route");
	fixture->remove_calls++;
	append_cleanup_trace(fixture, 'R', index);
	require(sol_evidence_run_note_disconnect_v1(fixture->run, index),
			"synchronous removal dispatches the evidence owner exactly once");
	if (index < SOL_KTX_CANDIDATE_COUNT_V1)
	{
		require(sol_candidate_registry_release_v1(fixture->registry, index),
				"candidate removal also releases its four-seat registry owner");
	}
}

static void test_pending_cleanup_keeps_complete_bot_frames_until_server_hook(void)
{
	timing_fixture_v1 fixture = { 0 };
	sol_candidate_frame_ops_v1 frame_ops = {
		&fixture, candidate_healthy, write_candidate_command
	};
	sol_ktx_command_v1 control_command = { 13u, { 0.0f, 0.0f, 0.0f },
		0, 0, 0, 0u, 0u };
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_runtime_schedule_decision_v1 schedule;
	size_t frame;
	size_t index;

	fixture.run = sol_evidence_run_create_v1();
	fixture.registry = sol_candidate_registry_create_v1();
	require(fixture.run != NULL && fixture.registry != NULL
		&& sol_evidence_run_begin_v1(fixture.run, run_nonce, "ktx-match/v1",
			fake_evidence_call, &fixture), "timing fixture begins one evidence epoch");
	bind_eight_live_seats(&fixture);
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
				"each intervening bot callback emits all four candidate commands");
		for (index = SOL_KTX_CANDIDATE_COUNT_V1;
			index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
		{
			submit_command(&fixture, index, &control_command);
		}
		fixture.bot_phase = 0;
		for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
		{
			require(fixture.request_calls[index] == (int) frame + 1
				&& fixture.command_calls[index] == (int) frame + 1,
					"all eight seats receive exactly one request and syscall per frame");
			if (index < SOL_KTX_CANDIDATE_COUNT_V1)
			{
				require(results[index].emitted,
					"all four candidate registry entries emitted through the hook");
			}
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
			remove_live_seat, &fixture) == SOL_EVIDENCE_CLEANUP_RETRY
		&& !strcmp(fixture.cleanup_trace, "EU1U2X3U4U5U6U7U8")
		&& fixture.end_calls == 1 && fixture.unbind_calls == 7
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
	for (index = 0; index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1; ++index)
	{
		require(fixture.request_calls[index] == 2
			&& fixture.command_calls[index] == 2,
				"bot callback after END attempt is fully inert during UNBIND retry");
		if (index < SOL_KTX_CANDIDATE_COUNT_V1)
		{
			require(fixture.observers[index].poll_calls == 0,
					"candidate COV polling is inert after END ownership");
		}
	}
	require(fixture.end_calls == 1 && fixture.unbind_calls == 7
		&& fixture.remove_calls == 0
		&& !strcmp(fixture.cleanup_trace, "EU1U2X3U4U5U6U7U8"),
			"intervening inert bot callback performs no cleanup operation");

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_SERVER_FRAME_V1,
		sol_evidence_run_active_v1(fixture.run),
		sol_evidence_run_emissions_open_v1(fixture.run), fixture.registry != NULL,
		sol_evidence_run_cleanup_pending_v1(fixture.run));
	require(!schedule.run_candidates && schedule.run_cleanup
		&& sol_evidence_run_server_cleanup_v1(fixture.run,
			remove_live_seat, &fixture) == SOL_EVIDENCE_CLEANUP_COMPLETE
		&& !strcmp(fixture.cleanup_trace,
			"EU1U2X3U4U5U6U7U8U3R1R2R3R4R5R6R7R8")
		&& fixture.end_calls == 1 && fixture.unbind_calls == 8
		&& fixture.remove_calls == 8,
			"later server callback retries only UNBIND then removes and completes");
	require(sol_evidence_run_begin_v1(fixture.run, second_run_nonce,
			"ktx-match/v1", fake_evidence_call, &fixture)
		&& fixture.begin_calls == 2,
			"successful eventual cleanup permits a reusable second epoch");
	sol_evidence_run_request_close_v1(fixture.run);
	require(sol_evidence_run_server_cleanup_v1(fixture.run,
			remove_live_seat, &fixture) == SOL_EVIDENCE_CLEANUP_COMPLETE,
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
