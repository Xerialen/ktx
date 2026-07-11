#include "sol_evidence_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum fake_phase_v1
{
	FAKE_IDLE = 0,
	FAKE_OPEN,
	FAKE_ENDED
} fake_phase_v1;

typedef struct fake_binding_v1
{
	int active;
	uint32_t slot;
	uint32_t generation;
} fake_binding_v1;

typedef struct fake_ce_v1
{
	sol_evidence_run_v1 *run;
	fake_phase_v1 phase;
	fake_binding_v1 bindings[SOL_EVIDENCE_RUN_SEATS_V1];
	char trace[128];
	size_t trace_length;
	int bot_phase;
	int begin_calls;
	int end_calls;
	int unbind_calls;
	int remove_calls;
	int fail_end_once;
	int reject_unbind_once[SOL_EVIDENCE_RUN_SEATS_V1];
	int reject_decision_once;
	int decision_calls;
	ce_frame_decision_v1 last_decision;
} fake_ce_v1;

static const char run_nonce[] =
	"1111111111111111111111111111111111111111111111111111111111111111";
static const char second_run_nonce[] =
	"2222222222222222222222222222222222222222222222222222222222222222";
static const char seat_nonces[SOL_EVIDENCE_RUN_SEATS_V1][CE_SEAT_NONCE_CAP] = {
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
	"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
	"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
	"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
	"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
	"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
};

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void trace_char(fake_ce_v1 *fake, char value)
{
	require(fake->trace_length + 1u < sizeof(fake->trace),
			"fake CE trace remains bounded");
	fake->trace[fake->trace_length++] = value;
	fake->trace[fake->trace_length] = '\0';
}

static intptr_t fake_call(void *context, intptr_t operation, void *payload,
	intptr_t payload_size)
{
	fake_ce_v1 *fake = context;

	if (operation == CE_MATCH_BEGIN)
	{
		ce_epoch_begin_v1 *begin = payload;

		require(!fake->bot_phase && fake->phase == FAKE_IDLE &&
			payload_size == (intptr_t) sizeof(*begin),
				"MATCH_BEGIN is legal only from the idle server phase");
		fake->phase = FAKE_OPEN;
		fake->begin_calls++;
		trace_char(fake, 'B');
		return CE_RESULT_OK;
	}
	if (operation == CE_MATCH_END)
	{
		ce_epoch_end_v1 *end = payload;

		require(!fake->bot_phase && fake->phase == FAKE_OPEN &&
			payload_size == (intptr_t) sizeof(*end),
				"MATCH_END is legal only from an open server phase");
		fake->phase = FAKE_ENDED;
		fake->end_calls++;
		trace_char(fake, 'E');
		if (fake->fail_end_once)
		{
			fake->fail_end_once = 0;
			return CE_RESULT_INVALID;
		}
		return CE_RESULT_OK;
	}
	if (operation == CE_FRAME_DECISION)
	{
		const ce_frame_decision_v1 *decision = payload;
		size_t index;

		require(fake->bot_phase && fake->phase == FAKE_OPEN &&
			payload_size == (intptr_t) sizeof(*decision) &&
			decision->header.protocol_version == CE_PROTOCOL_VERSION_V1 &&
			decision->header.struct_size == sizeof(*decision),
			"FRAME_DECISION is one exact bot-phase ABI payload");
		require(decision->action_response_length > 0u &&
			decision->action_response_length <=
				CE_ACTION_RESPONSE_MAX_BYTES_V1 &&
			decision->decision_trace_length > 0u &&
			decision->decision_trace_length <=
				CE_DECISION_TRACE_MAX_BYTES_V1,
			"FRAME_DECISION lengths are bounded before the syscall");
		for (index = decision->action_response_length;
			index < CE_ACTION_RESPONSE_MAX_BYTES_V1; ++index)
		{
			require(decision->action_response[index] == 0u,
				"unused SAC1 payload tail is canonical zero");
		}
		for (index = decision->decision_trace_length;
			index < CE_DECISION_TRACE_MAX_BYTES_V1; ++index)
		{
			require(decision->decision_trace[index] == 0u,
				"unused SDT1 payload tail is canonical zero");
		}
		fake->last_decision = *decision;
		fake->decision_calls++;
		trace_char(fake, 'D');
		if (fake->reject_decision_once)
		{
			fake->reject_decision_once = 0;
			return CE_RESULT_INVALID;
		}
		return CE_RESULT_OK;
	}
	if (operation == CE_UNBIND)
	{
		ce_unbind_v1 *unbind = payload;
		size_t index;

		require(!fake->bot_phase && fake->phase == FAKE_ENDED &&
			payload_size == (intptr_t) sizeof(*unbind),
				"UNBIND is legal only after end in the server phase");
		for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
		{
			if (fake->bindings[index].active &&
				fake->bindings[index].slot == unbind->engine_slot &&
				fake->bindings[index].generation == unbind->client_generation)
			{
				if (fake->reject_unbind_once[index])
				{
					fake->reject_unbind_once[index] = 0;
					trace_char(fake, 'x');
					return CE_RESULT_INVALID;
				}
				fake->bindings[index].active = 0;
				fake->unbind_calls++;
				trace_char(fake, '1' + (char) index);
				break;
			}
		}
		require(index < SOL_EVIDENCE_RUN_SEATS_V1,
				"UNBIND uses the exact successful CE route");
		for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
		{
			if (fake->bindings[index].active)
			{
				return CE_RESULT_OK;
			}
		}
		fake->phase = FAKE_IDLE;
		return CE_RESULT_OK;
	}
	return CE_RESULT_INVALID;
}

static void fake_bind(fake_ce_v1 *fake, size_t index, uint32_t slot,
	uint32_t generation)
{
	require(fake->phase == FAKE_OPEN && index < SOL_EVIDENCE_RUN_SEATS_V1,
			"fake bind belongs to one open epoch");
	fake->bindings[index].active = 1;
	fake->bindings[index].slot = slot;
	fake->bindings[index].generation = generation;
}

static void fake_remove(void *context, size_t index, uint32_t engine_slot)
{
	fake_ce_v1 *fake = context;

	require(!fake->bot_phase && engine_slot == 7u + (uint32_t) index,
			"candidate removal uses the stored client route in server phase");
	fake->remove_calls++;
	trace_char(fake, 'R');
	trace_char(fake, '1' + (char) index);
	require(sol_evidence_run_note_disconnect_v1(fake->run, index),
			"synchronous RemoveBot disconnect is accepted after unbind");
}

static void configure_bound_seat(fake_ce_v1 *fake, size_t index)
{
	uint32_t slot = 7u + (uint32_t) index;
	uint32_t generation = 100u + (uint32_t) index;

	require(sol_evidence_run_configure_seat_v1(fake->run, index,
			seat_nonces[index]), "seat config is retained by the evidence run");
	require(sol_evidence_run_record_client_v1(fake->run, index, slot),
			"client route is recorded before CE bind");
	fake_bind(fake, index, slot, generation);
	require(sol_evidence_run_record_bind_v1(fake->run, index, slot, generation) ==
			SOL_EVIDENCE_BIND_ACCEPTED,
			"successful CE bind route is retained immediately");
}

static void test_normal_close_waits_for_server_phase_and_is_reusable(void)
{
	fake_ce_v1 fake = { 0 };
	sol_evidence_cleanup_result_v1 cleanup;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL, "evidence run allocation succeeds");
	require(sol_evidence_run_begin_v1(fake.run, run_nonce, "ktx-match/v1",
			fake_call, &fake), "first evidence epoch begins");
	configure_bound_seat(&fake, 3u);
	configure_bound_seat(&fake, 1u);
	configure_bound_seat(&fake, 0u);
	configure_bound_seat(&fake, 2u);
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.bot_phase = 1;
	sol_evidence_run_request_close_v1(fake.run);
	require(!sol_evidence_run_failed_v1(fake.run) && fake.end_calls == 0 &&
		fake.unbind_calls == 0 && fake.remove_calls == 0,
			"bot phase only marks close pending and performs no cleanup operation");
	fake.bot_phase = 0;
	cleanup = sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake);
	require(cleanup == SOL_EVIDENCE_CLEANUP_COMPLETE &&
			!strcmp(fake.trace, "E1234R1R2R3R4"),
			"server phase performs end, every unbind, then synchronous removals");
	require(!sol_evidence_run_active_v1(fake.run),
			"complete cleanup makes the local run reusable");
	require(sol_evidence_run_begin_v1(fake.run, second_run_nonce, "ktx-match/v1",
			fake_call, &fake),
			"normal cleanup and successful unbind permit a second run");
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE,
			"empty second run closes cleanly");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_invalid_end_still_unbinds_and_allows_second_run(void)
{
	fake_ce_v1 fake = { 0 };

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "failed-end fixture begins");
	configure_bound_seat(&fake, 0u);
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.fail_end_once = 1;
	fake.bot_phase = 1;
	sol_evidence_run_fail_stop_v1(fake.run);
	require(sol_evidence_run_failed_v1(fake.run) && fake.end_calls == 0 &&
		fake.unbind_calls == 0,
			"bot fail-stop latches failure while deferring cleanup ownership");
	fake.bot_phase = 0;
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "E1R1"),
			"INVALID end that transitioned engine state still unbinds and removes");
	require(sol_evidence_run_begin_v1(fake.run, second_run_nonce, "ktx-match/v1",
			fake_call, &fake),
			"failed end followed by successful unbind permits a second run");
	require(!sol_evidence_run_failed_v1(fake.run),
			"a new evidence epoch clears the prior failure latch");
	configure_bound_seat(&fake, 0u);
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE,
			"second epoch reuses and cleans the same engine slot");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_rejected_unbind_retains_route_until_retry_succeeds(void)
{
	fake_ce_v1 fake = { 0 };
	uint32_t slot = 0u;
	uint32_t generation = 0u;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "unbind-retry fixture begins");
	configure_bound_seat(&fake, 0u);
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.reject_unbind_once[0] = 1;
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_RETRY && !strcmp(fake.trace, "Ex") &&
			fake.remove_calls == 0,
			"rejected unbind retains fail-closed state and removes no client");
	require(sol_evidence_run_binding_v1(fake.run, 0u, &slot, &generation) &&
			slot == 7u && generation == 100u,
			"rejected unbind preserves the exact successful binding route");
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "Ex1R1") &&
			fake.end_calls == 1,
			"later server frame retries unbind without replaying MATCH_END");
	require(sol_evidence_run_begin_v1(fake.run, second_run_nonce, "ktx-match/v1",
			fake_call, &fake), "successful retry makes the next run reusable");
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE, "unbind-retry second run closes");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_unexpected_disconnect_keeps_binding_for_safe_unbind(void)
{
	fake_ce_v1 fake = { 0 };
	uint32_t slot = 0u;
	uint32_t generation = 0u;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "disconnect fixture begins");
	configure_bound_seat(&fake, 0u);
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.bot_phase = 1;
	require(sol_evidence_run_note_disconnect_v1(fake.run, 0u) &&
			sol_evidence_run_cleanup_pending_v1(fake.run) &&
			sol_evidence_run_failed_v1(fake.run) &&
			sol_evidence_run_binding_v1(fake.run, 0u, &slot, &generation) &&
			slot == 7u && generation == 100u &&
			sol_evidence_run_command_route_v1(fake.run, 7u, NULL) ==
				SOL_EVIDENCE_COMMAND_QUARANTINED,
			"unexpected disconnect retains bind route and marks whole run pending");
	require(fake.end_calls == 0 && fake.unbind_calls == 0,
			"disconnect in active bot phase performs no CE cleanup");
	fake.bot_phase = 0;
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "E1") &&
			fake.remove_calls == 0,
			"safe server hook unbinds disconnected route without removing reused slot");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_prebind_failure_ends_epoch_before_removing_client(void)
{
	fake_ce_v1 fake = { 0 };

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "prebind-failure fixture begins");
	require(sol_evidence_run_configure_seat_v1(fake.run, 0u, seat_nonces[0]) &&
			sol_evidence_run_record_client_v1(fake.run, 0u, 7u),
			"prebind client route is retained for deferred removal");
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.bot_phase = 1;
	sol_evidence_run_fail_stop_v1(fake.run);
	require(fake.end_calls == 0 && fake.remove_calls == 0,
			"prebind failure performs no bot-phase cleanup");
	fake.bot_phase = 0;
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "ER1") &&
			fake.unbind_calls == 0,
			"prebind failure ends epoch before removing only its known client");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_config_and_client_route_reject_ambiguity(void)
{
	fake_ce_v1 fake = { 0 };

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "client-route fixture begins");
	require(sol_evidence_run_configure_seat_v1(fake.run, 0u, seat_nonces[0]) &&
			sol_evidence_run_configure_seat_v1(fake.run, 1u, seat_nonces[1]),
			"two distinct seats configure for client-route invariants");
	require(!sol_evidence_run_configure_seat_v1(fake.run, 2u, seat_nonces[1]),
			"duplicate seat nonce is rejected");
	require(sol_evidence_run_record_client_v1(fake.run, 0u, 7u),
			"first live client route records");
	require(!sol_evidence_run_record_client_v1(fake.run, 1u, 7u),
			"duplicate live engine slot is rejected across seats");
	require(sol_evidence_run_record_bind_v1(fake.run, 0u, 7u, 0u) ==
			SOL_EVIDENCE_BIND_REJECTED,
			"bind generation zero is rejected");
	sol_evidence_run_fail_stop_v1(fake.run);
	require(!sol_evidence_run_record_client_v1(fake.run, 1u, 8u),
			"cleanup-pending run rejects a late pre-bind client route");
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "BER1"),
			"client-route fixture closes without inventing an invalid bind route");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_mismatched_successful_bind_keeps_both_cleanup_routes(void)
{
	fake_ce_v1 fake = { 0 };
	uint32_t slot = 0u;
	uint32_t generation = 0u;
	size_t index = SOL_EVIDENCE_RUN_SEATS_V1;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "mismatched-bind fixture begins");
	require(sol_evidence_run_configure_seat_v1(fake.run, 0u, seat_nonces[0]) &&
			sol_evidence_run_record_client_v1(fake.run, 0u, 7u),
			"original client removal route records before CE bind");
	fake_bind(&fake, 0u, 8u, 100u);
	require(sol_evidence_run_record_bind_v1(fake.run, 0u, 8u, 100u) ==
			SOL_EVIDENCE_BIND_RETAINED_CONFLICT,
			"mismatched successful CE route reports fail-stop");
	require(sol_evidence_run_binding_v1(fake.run, 0u, &slot, &generation) &&
			slot == 8u && generation == 100u,
			"mismatched successful CE route remains authoritative for UNBIND");
	require(sol_evidence_run_command_route_v1(fake.run, 8u, &generation) ==
			SOL_EVIDENCE_COMMAND_BOUND && generation == 100u &&
		sol_evidence_run_command_route_v1(fake.run, 7u, NULL) ==
			SOL_EVIDENCE_COMMAND_QUARANTINED,
			"open mismatch owns its bind route and quarantines its unbound client route");
	require(!sol_evidence_run_find_client_v1(fake.run, 8u, &index) &&
			index == SOL_EVIDENCE_RUN_SEATS_V1 &&
			!sol_evidence_run_cleanup_pending_v1(fake.run),
			"an unrelated bind-slot disconnect cannot erase the original client route");
	require(sol_evidence_run_find_client_v1(fake.run, 7u, &index) && index == 0u,
			"original client route remains authoritative for removal");
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	sol_evidence_run_fail_stop_v1(fake.run);
	require(sol_evidence_run_command_route_v1(fake.run, 8u, &generation) ==
			SOL_EVIDENCE_COMMAND_QUARANTINED && generation == 100u &&
		sol_evidence_run_command_route_v1(fake.run, 7u, NULL) ==
			SOL_EVIDENCE_COMMAND_QUARANTINED &&
		sol_evidence_run_command_route_v1(fake.run, 9u, NULL) ==
			SOL_EVIDENCE_COMMAND_UNOWNED,
			"latched failure quarantines both mismatched ownership routes only");
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "E1R1"),
			"cleanup unbinds CE slot then removes the distinct client slot");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_duplicate_successful_ce_routes_are_all_retained(void)
{
	fake_ce_v1 fake = { 0 };
	uint32_t slot = 0u;
	uint32_t generation = 0u;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "duplicate-bind fixture begins");
	require(sol_evidence_run_configure_seat_v1(fake.run, 0u, seat_nonces[0]) &&
			sol_evidence_run_configure_seat_v1(fake.run, 1u, seat_nonces[1]) &&
			sol_evidence_run_record_client_v1(fake.run, 0u, 7u) &&
			sol_evidence_run_record_client_v1(fake.run, 1u, 8u),
			"two distinct original client routes record");
	fake_bind(&fake, 0u, 7u, 100u);
	require(sol_evidence_run_record_bind_v1(fake.run, 0u, 7u, 100u) ==
			SOL_EVIDENCE_BIND_ACCEPTED,
			"first exact CE route records");
	fake_bind(&fake, 1u, 7u, 101u);
	require(sol_evidence_run_record_bind_v1(fake.run, 1u, 7u, 101u) ==
			SOL_EVIDENCE_BIND_RETAINED_CONFLICT,
			"duplicate CE slot reports fail-stop");
	require(sol_evidence_run_binding_v1(fake.run, 1u, &slot, &generation) &&
			slot == 7u && generation == 101u,
			"duplicate successful CE route is retained despite rejection result");
	require(sol_evidence_run_find_binding_v1(fake.run, 7u, NULL, NULL) ==
			SOL_EVIDENCE_BINDING_AMBIGUOUS,
			"duplicate successful CE slot is never exposed as an exact command route");
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	sol_evidence_run_fail_stop_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE && !strcmp(fake.trace, "E12R1R2"),
			"cleanup attempts every successful CE route before client removals");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_late_successful_bind_is_retained_while_reporting_fail_stop(void)
{
	fake_ce_v1 fake = { 0 };
	uint32_t slot = 0u;
	uint32_t generation = 0u;

	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "late-bind fixture begins");
	require(sol_evidence_run_configure_seat_v1(fake.run, 0u, seat_nonces[0]) &&
			sol_evidence_run_record_client_v1(fake.run, 0u, 7u),
			"late-bind original client route records");
	fake_bind(&fake, 0u, 7u, 100u);
	sol_evidence_run_fail_stop_v1(fake.run);
	require(sol_evidence_run_record_bind_v1(fake.run, 0u, 7u, 100u) ==
			SOL_EVIDENCE_BIND_RETAINED_CONFLICT,
			"cleanup-pending successful bind reports fail-stop");
	require(sol_evidence_run_binding_v1(fake.run, 0u, &slot, &generation) &&
			slot == 7u && generation == 100u,
			"cleanup-pending successful bind still records its UNBIND route");
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE,
			"late successful bind is safely unbound before client removal");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_all_eight_generic_seats_bind_cleanup_and_reuse(void)
{
	static const size_t launch_order[SOL_EVIDENCE_RUN_SEATS_V1] = {
		0u, 4u, 1u, 5u, 2u, 6u, 3u, 7u
	};
	fake_ce_v1 fake = { 0 };
	size_t launched;

	require(SOL_EVIDENCE_RUN_SEATS_V1 == 8,
			"generic evidence lifecycle owns exactly eight seats");
	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
			"ktx-match/v1", fake_call, &fake), "eight-seat fixture begins");
	for (launched = 0; launched < SOL_EVIDENCE_RUN_SEATS_V1; ++launched)
	{
		size_t index = launch_order[launched];
		size_t found = SOL_EVIDENCE_RUN_SEATS_V1;
		uint32_t generation = 0u;

		configure_bound_seat(&fake, index);
		require(sol_evidence_run_find_binding_v1(fake.run,
				7u + (uint32_t) index, &found, &generation) ==
				SOL_EVIDENCE_BINDING_EXACT && found == index
			&& generation == 100u + (uint32_t) index,
				"each alternating launch exposes one unique exact command route");
	}
	require(sol_evidence_run_find_binding_v1(fake.run, 31u, NULL, NULL) ==
			SOL_EVIDENCE_BINDING_NONE,
			"unbound engine slot remains an exact bypass");
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE
		&& !strcmp(fake.trace,
			"E12345678R1R2R3R4R5R6R7R8"),
			"all eight routes END, UNBIND, and remove in canonical seat order");
	require(sol_evidence_run_begin_v1(fake.run, second_run_nonce, "ktx-match/v1",
			fake_call, &fake), "eight-seat cleanup permits a reusable second epoch");
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
			SOL_EVIDENCE_CLEANUP_COMPLETE, "empty reused epoch closes cleanly");
	sol_evidence_run_destroy_v1(fake.run);
}

static void test_candidate_decision_uses_bound_route_and_zeroed_fixed_payload(void)
{
	fake_ce_v1 fake = { 0 };
	uint8_t action[33];
	uint8_t trace[79];
	int calls;

	memset(action, 0x5a, sizeof(action));
	memset(trace, 0xa5, sizeof(trace));
	fake.run = sol_evidence_run_create_v1();
	require(fake.run != NULL && sol_evidence_run_begin_v1(fake.run, run_nonce,
		"ktx-match/v1", fake_call, &fake), "decision fixture begins");
	configure_bound_seat(&fake, 0u);
	configure_bound_seat(&fake, 4u);
	fake.trace_length = 0u;
	fake.trace[0] = '\0';
	fake.bot_phase = 1;
	require(sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
		sizeof(action), trace, sizeof(trace)) && fake.decision_calls == 1 &&
		!strcmp(fake.trace, "D") &&
		fake.last_decision.engine_slot == 7u &&
		fake.last_decision.client_generation == 100u &&
		fake.last_decision.action_response_length == sizeof(action) &&
		fake.last_decision.decision_trace_length == sizeof(trace) &&
		!memcmp(fake.last_decision.action_response, action, sizeof(action)) &&
		!memcmp(fake.last_decision.decision_trace, trace, sizeof(trace)),
		"candidate decision submits exact bytes through its retained CE route");
	calls = fake.decision_calls;
	require(!sol_evidence_run_submit_decision_v1(fake.run, 4u, action,
		sizeof(action), trace, sizeof(trace)) &&
		fake.decision_calls == calls,
		"control evidence seats cannot submit SOL decisions");
	require(!sol_evidence_run_submit_decision_v1(fake.run, 1u, action,
		sizeof(action), trace, sizeof(trace)) &&
		!sol_evidence_run_submit_decision_v1(fake.run, 0u, action, 0u,
			trace, sizeof(trace)) &&
		!sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
			CE_ACTION_RESPONSE_MAX_BYTES_V1 + 1u, trace, sizeof(trace)) &&
		!sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
			sizeof(action), trace, CE_DECISION_TRACE_MAX_BYTES_V1 + 1u) &&
		fake.decision_calls == calls,
		"unbound and noncanonical decision payloads never reach the engine");
	fake.reject_decision_once = 1;
	require(!sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
		sizeof(action), trace, sizeof(trace)) &&
		fake.decision_calls == calls + 1,
		"engine decision rejection is surfaced to the runtime");
	sol_evidence_run_request_close_v1(fake.run);
	require(sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
		sizeof(action), trace, sizeof(trace)) &&
		fake.decision_calls == calls + 2,
		"cleanup-pending bot cadence remains complete until server END owns it");
	sol_evidence_run_fail_stop_v1(fake.run);
	require(sol_evidence_run_failed_v1(fake.run) &&
		!sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
			sizeof(action), trace, sizeof(trace)) &&
		fake.decision_calls == calls + 2,
		"latched failure admits no later active decision proof before cleanup");
	fake.bot_phase = 0;
	require(sol_evidence_run_server_cleanup_v1(fake.run, fake_remove, &fake) ==
		SOL_EVIDENCE_CLEANUP_COMPLETE,
		"decision fixture cleans both retained routes");
	require(!sol_evidence_run_submit_decision_v1(fake.run, 0u, action,
		sizeof(action), trace, sizeof(trace)) &&
		fake.decision_calls == calls + 2,
		"completed server cleanup admits no later decision submission");
	sol_evidence_run_destroy_v1(fake.run);
}

int main(void)
{
	test_normal_close_waits_for_server_phase_and_is_reusable();
	test_invalid_end_still_unbinds_and_allows_second_run();
	test_rejected_unbind_retains_route_until_retry_succeeds();
	test_unexpected_disconnect_keeps_binding_for_safe_unbind();
	test_prebind_failure_ends_epoch_before_removing_client();
	test_config_and_client_route_reject_ambiguity();
	test_mismatched_successful_bind_keeps_both_cleanup_routes();
	test_duplicate_successful_ce_routes_are_all_retained();
	test_late_successful_bind_is_retained_while_reporting_fail_stop();
	test_all_eight_generic_seats_bind_cleanup_and_reuse();
	test_candidate_decision_uses_bound_route_and_zeroed_fixed_payload();
	printf("sol_evidence_run: 11 contract tests passed\n");
	return 0;
}
