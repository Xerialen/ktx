#include "sol_evidence_run.h"

#include <stdlib.h>
#include <string.h>

typedef struct sol_evidence_run_seat_v1
{
	int configured;
	int client_present;
	int ce_bound;
	uint32_t client_slot;
	uint32_t bind_engine_slot;
	uint32_t client_generation;
	char seat_nonce[CE_SEAT_NONCE_CAP];
} sol_evidence_run_seat_v1;

struct sol_evidence_run_v1
{
	int active;
	int emissions_open;
	int cleanup_pending;
	int failed;
	int end_attempted;
	int match_timeline_started;
	char run_nonce[CE_RUN_NONCE_CAP];
	char epoch_kind[CE_EPOCH_KIND_CAP];
	sol_evidence_run_call_v1 call;
	void *call_context;
	sol_evidence_run_seat_v1 seats[SOL_EVIDENCE_RUN_SEATS_V1];
};

static void init_header(ce_payload_header_v1 *header, size_t size)
{
	header->protocol_version = CE_PROTOCOL_VERSION_V1;
	header->struct_size = (uint32_t) size;
}

static int copy_string(char *output, size_t capacity, const char *value)
{
	size_t length;

	if (!output || !capacity || !value)
	{
		return 0;
	}
	length = strlen(value);
	if (!length || length >= capacity)
	{
		return 0;
	}
	memcpy(output, value, length + 1u);
	return 1;
}

sol_evidence_run_v1 *sol_evidence_run_create_v1(void)
{
	return calloc(1u, sizeof(sol_evidence_run_v1));
}

void sol_evidence_run_destroy_v1(sol_evidence_run_v1 *run)
{
	if (run)
	{
		memset(run, 0, sizeof(*run));
	}
	free(run);
}

int sol_evidence_run_begin_v1(sol_evidence_run_v1 *run,
	const char *run_nonce, const char *epoch_kind,
	sol_evidence_run_call_v1 call, void *call_context)
{
	ce_epoch_begin_v1 begin;

	if (!run || run->active || !call)
	{
		return 0;
	}
	memset(&begin, 0, sizeof(begin));
	init_header(&begin.header, sizeof(begin));
	if (!copy_string(begin.run_nonce, sizeof(begin.run_nonce), run_nonce)
		|| !copy_string(begin.epoch_kind, sizeof(begin.epoch_kind), epoch_kind))
	{
		return 0;
	}
	begin.epoch_id = 1u;
	if (call(call_context, CE_MATCH_BEGIN, &begin,
		(intptr_t) sizeof(begin)) != CE_RESULT_OK)
	{
		return 0;
	}
	memset(run, 0, sizeof(*run));
	run->active = 1;
	run->emissions_open = 1;
	run->call = call;
	run->call_context = call_context;
	copy_string(run->run_nonce, sizeof(run->run_nonce), run_nonce);
	copy_string(run->epoch_kind, sizeof(run->epoch_kind), epoch_kind);
	return 1;
}

int sol_evidence_run_matches_v1(const sol_evidence_run_v1 *run,
	const char *run_nonce, const char *epoch_kind)
{
	return run && run->active && run_nonce && epoch_kind
		&& !strcmp(run->run_nonce, run_nonce)
		&& !strcmp(run->epoch_kind, epoch_kind);
}

const char *sol_evidence_run_nonce_v1(const sol_evidence_run_v1 *run)
{
	return run && run->active ? run->run_nonce : NULL;
}

int sol_evidence_run_match_timeline_begin_v1(sol_evidence_run_v1 *run)
{
	ce_match_timeline_begin_v1 timeline;

	if (!run || !run->active || !run->emissions_open || run->cleanup_pending
		|| run->match_timeline_started
		|| strcmp(run->epoch_kind, "ktx-match/v1"))
	{
		return 0;
	}
	memset(&timeline, 0, sizeof(timeline));
	init_header(&timeline.header, sizeof(timeline));
	if (!copy_string(timeline.run_nonce, sizeof(timeline.run_nonce),
			run->run_nonce)
		|| run->call(run->call_context, CE_MATCH_TIMELINE_BEGIN, &timeline,
			(intptr_t) sizeof(timeline)) != CE_RESULT_OK
		|| timeline.header.protocol_version != CE_PROTOCOL_VERSION_V1
		|| timeline.header.struct_size != sizeof(timeline)
		|| strcmp(timeline.run_nonce, run->run_nonce)
		|| !timeline.mvd_time_us)
	{
		return 0;
	}
	run->match_timeline_started = 1;
	return 1;
}

int sol_evidence_run_configure_seat_v1(sol_evidence_run_v1 *run,
	size_t index, const char *seat_nonce)
{
	sol_evidence_run_seat_v1 *seat;
	char candidate_nonce[CE_SEAT_NONCE_CAP];
	size_t candidate;

	if (!run || !run->active || run->cleanup_pending
		|| index >= SOL_EVIDENCE_RUN_SEATS_V1)
	{
		return 0;
	}
	seat = &run->seats[index];
	if (seat->configured || !copy_string(candidate_nonce,
			sizeof(candidate_nonce), seat_nonce))
	{
		return 0;
	}
	for (candidate = 0; candidate < SOL_EVIDENCE_RUN_SEATS_V1; ++candidate)
	{
		if (run->seats[candidate].configured
			&& !strcmp(run->seats[candidate].seat_nonce, candidate_nonce))
		{
			return 0;
		}
	}
	copy_string(seat->seat_nonce, sizeof(seat->seat_nonce), candidate_nonce);
	seat->configured = 1;
	return 1;
}

int sol_evidence_run_seat_configured_v1(const sol_evidence_run_v1 *run,
	size_t index)
{
	return run && run->active && index < SOL_EVIDENCE_RUN_SEATS_V1
		&& run->seats[index].configured;
}

static int slot_used_by_other_seat(const sol_evidence_run_v1 *run,
	size_t own_index, uint32_t engine_slot)
{
	size_t index;

	for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
	{
		const sol_evidence_run_seat_v1 *seat = &run->seats[index];

		if (index == own_index)
		{
			continue;
		}
		if ((seat->client_present && seat->client_slot == engine_slot)
			|| (seat->ce_bound && seat->bind_engine_slot == engine_slot))
		{
			return 1;
		}
	}
	return 0;
}

int sol_evidence_run_record_client_v1(sol_evidence_run_v1 *run,
	size_t index, uint32_t engine_slot)
{
	sol_evidence_run_seat_v1 *seat;

	if (!run || !run->active || run->cleanup_pending || !engine_slot ||
		index >= SOL_EVIDENCE_RUN_SEATS_V1)
	{
		return 0;
	}
	seat = &run->seats[index];
	if (!seat->configured || seat->client_present || seat->ce_bound
		|| slot_used_by_other_seat(run, index, engine_slot))
	{
		return 0;
	}
	seat->client_slot = engine_slot;
	seat->client_present = 1;
	return 1;
}

sol_evidence_bind_record_result_v1 sol_evidence_run_record_bind_v1(
	sol_evidence_run_v1 *run, size_t index, uint32_t engine_slot,
	uint32_t client_generation)
{
	sol_evidence_run_seat_v1 *seat;
	int route_matches_client;
	int unique_slot;
	int accepted_before_cleanup;

	if (!run || !run->active || !engine_slot || !client_generation
		|| index >= SOL_EVIDENCE_RUN_SEATS_V1)
	{
		return SOL_EVIDENCE_BIND_REJECTED;
	}
	seat = &run->seats[index];
	if (!seat->configured || seat->ce_bound)
	{
		return SOL_EVIDENCE_BIND_REJECTED;
	}
	route_matches_client = seat->client_present
		&& seat->client_slot == engine_slot;
	unique_slot = !slot_used_by_other_seat(run, index, engine_slot);
	accepted_before_cleanup = !run->cleanup_pending;

	/*
	 * CE_BIND has already succeeded when this is called.  Persist its exact
	 * route before reporting any local consistency failure: the safe server
	 * hook must retain authority to unbind it even when the client route,
	 * another seat, or a re-entrant failure has made the run fail-stop.
	 */
	seat->bind_engine_slot = engine_slot;
	seat->client_generation = client_generation;
	seat->ce_bound = 1;
	return route_matches_client && unique_slot && accepted_before_cleanup ?
		SOL_EVIDENCE_BIND_ACCEPTED : SOL_EVIDENCE_BIND_RETAINED_CONFLICT;
}

int sol_evidence_run_find_client_v1(const sol_evidence_run_v1 *run,
	uint32_t engine_slot, size_t *index)
{
	size_t candidate;

	if (index)
	{
		*index = SOL_EVIDENCE_RUN_SEATS_V1;
	}
	if (!run || !run->active || !engine_slot)
	{
		return 0;
	}
	for (candidate = 0; candidate < SOL_EVIDENCE_RUN_SEATS_V1; ++candidate)
	{
		const sol_evidence_run_seat_v1 *seat = &run->seats[candidate];

		if (seat->configured && seat->client_present
			&& seat->client_slot == engine_slot)
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

int sol_evidence_run_binding_v1(const sol_evidence_run_v1 *run,
	size_t index, uint32_t *engine_slot, uint32_t *client_generation)
{
	const sol_evidence_run_seat_v1 *seat;

	if (!run || !run->active || index >= SOL_EVIDENCE_RUN_SEATS_V1)
	{
		return 0;
	}
	seat = &run->seats[index];
	if (!seat->ce_bound)
	{
		return 0;
	}
	if (engine_slot)
	{
		*engine_slot = seat->bind_engine_slot;
	}
	if (client_generation)
	{
		*client_generation = seat->client_generation;
	}
	return 1;
}

sol_evidence_binding_lookup_result_v1 sol_evidence_run_find_binding_v1(
	const sol_evidence_run_v1 *run, uint32_t engine_slot, size_t *index,
	uint32_t *client_generation)
{
	size_t candidate;
	size_t found = SOL_EVIDENCE_RUN_SEATS_V1;

	if (index)
	{
		*index = SOL_EVIDENCE_RUN_SEATS_V1;
	}
	if (client_generation)
	{
		*client_generation = 0u;
	}
	if (!run || !run->active || !engine_slot)
	{
		return SOL_EVIDENCE_BINDING_NONE;
	}
	for (candidate = 0; candidate < SOL_EVIDENCE_RUN_SEATS_V1; ++candidate)
	{
		const sol_evidence_run_seat_v1 *seat = &run->seats[candidate];

		if (!seat->ce_bound || seat->bind_engine_slot != engine_slot)
		{
			continue;
		}
		if (found != SOL_EVIDENCE_RUN_SEATS_V1)
		{
			return SOL_EVIDENCE_BINDING_AMBIGUOUS;
		}
		found = candidate;
	}
	if (found == SOL_EVIDENCE_RUN_SEATS_V1)
	{
		return SOL_EVIDENCE_BINDING_NONE;
	}
	if (index)
	{
		*index = found;
	}
	if (client_generation)
	{
		*client_generation = run->seats[found].client_generation;
	}
	return SOL_EVIDENCE_BINDING_EXACT;
}

sol_evidence_command_route_v1 sol_evidence_run_command_route_v1(
	const sol_evidence_run_v1 *run, uint32_t engine_slot,
	uint32_t *client_generation)
{
	sol_evidence_binding_lookup_result_v1 binding;
	int client_owned;

	binding = sol_evidence_run_find_binding_v1(run, engine_slot, NULL,
		client_generation);
	client_owned = sol_evidence_run_find_client_v1(run, engine_slot, NULL);
	if (!run || !run->active)
	{
		return SOL_EVIDENCE_COMMAND_UNOWNED;
	}
	if (run->failed || !run->emissions_open)
	{
		return client_owned || binding != SOL_EVIDENCE_BINDING_NONE ?
			SOL_EVIDENCE_COMMAND_QUARANTINED : SOL_EVIDENCE_COMMAND_UNOWNED;
	}
	if (binding == SOL_EVIDENCE_BINDING_EXACT)
	{
		return SOL_EVIDENCE_COMMAND_BOUND;
	}
	if (binding == SOL_EVIDENCE_BINDING_AMBIGUOUS)
	{
		return SOL_EVIDENCE_COMMAND_AMBIGUOUS;
	}
	return client_owned ? SOL_EVIDENCE_COMMAND_QUARANTINED :
		SOL_EVIDENCE_COMMAND_UNOWNED;
}

int sol_evidence_run_submit_decision_v1(sol_evidence_run_v1 *run,
	size_t index, const uint8_t *action_response,
	size_t action_response_length, const uint8_t *decision_trace,
	size_t decision_trace_length)
{
	ce_frame_decision_v1 decision = {0};
	const sol_evidence_run_seat_v1 *seat;

	if (!run || !run->active || !run->emissions_open || run->failed
		|| !run->call || index >= SOL_EVIDENCE_RUN_CANDIDATE_SEATS_V1
		|| !action_response || !action_response_length
		|| action_response_length > CE_ACTION_RESPONSE_MAX_BYTES_V1
		|| !decision_trace || !decision_trace_length
		|| decision_trace_length > CE_DECISION_TRACE_MAX_BYTES_V1)
	{
		return 0;
	}
	seat = &run->seats[index];
	if (!seat->ce_bound || !seat->bind_engine_slot || !seat->client_generation)
	{
		return 0;
	}
	init_header(&decision.header, sizeof(decision));
	decision.engine_slot = seat->bind_engine_slot;
	decision.client_generation = seat->client_generation;
	decision.action_response_length = (uint32_t) action_response_length;
	decision.decision_trace_length = (uint32_t) decision_trace_length;
	memcpy(decision.action_response, action_response, action_response_length);
	memcpy(decision.decision_trace, decision_trace, decision_trace_length);
	return run->call(run->call_context, CE_FRAME_DECISION, &decision,
		(intptr_t) sizeof(decision)) == CE_RESULT_OK;
}

void sol_evidence_run_request_close_v1(sol_evidence_run_v1 *run)
{
	if (run && run->active)
	{
		run->cleanup_pending = 1;
	}
}

void sol_evidence_run_fail_stop_v1(sol_evidence_run_v1 *run)
{
	if (run && run->active)
	{
		run->failed = 1;
		run->cleanup_pending = 1;
	}
}

int sol_evidence_run_note_disconnect_v1(sol_evidence_run_v1 *run,
	size_t index)
{
	sol_evidence_run_seat_v1 *seat;

	if (!run || !run->active || index >= SOL_EVIDENCE_RUN_SEATS_V1
		|| !run->seats[index].configured)
	{
		return 0;
	}
	seat = &run->seats[index];
	seat->client_present = 0;
	if (!run->end_attempted)
	{
		run->failed = 1;
	}
	if (!(run->end_attempted && !seat->ce_bound))
	{
		run->cleanup_pending = 1;
	}
	return 1;
}

int sol_evidence_run_active_v1(const sol_evidence_run_v1 *run)
{
	return run && run->active;
}

int sol_evidence_run_emissions_open_v1(const sol_evidence_run_v1 *run)
{
	return run && run->active && run->emissions_open;
}

int sol_evidence_run_cleanup_pending_v1(const sol_evidence_run_v1 *run)
{
	return run && run->active && run->cleanup_pending;
}

int sol_evidence_run_failed_v1(const sol_evidence_run_v1 *run)
{
	return run && run->active && run->failed;
}

static int any_bound(const sol_evidence_run_v1 *run)
{
	size_t index;

	for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
	{
		if (run->seats[index].ce_bound)
		{
			return 1;
		}
	}
	return 0;
}

sol_evidence_cleanup_result_v1 sol_evidence_run_server_cleanup_v1(
	sol_evidence_run_v1 *run, sol_evidence_run_remove_v1 remove,
	void *remove_context)
{
	ce_epoch_end_v1 end;
	size_t index;

	if (!run || !run->active || !run->cleanup_pending)
	{
		return SOL_EVIDENCE_CLEANUP_IDLE;
	}
	/* END ownership closes bot emissions even when later UNBIND must retry. */
	run->emissions_open = 0;
	if (!run->end_attempted)
	{
		memset(&end, 0, sizeof(end));
		init_header(&end.header, sizeof(end));
		copy_string(end.run_nonce, sizeof(end.run_nonce), run->run_nonce);
		(void) run->call(run->call_context, CE_MATCH_END, &end,
			(intptr_t) sizeof(end));
		run->end_attempted = 1;
	}
	for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
	{
		sol_evidence_run_seat_v1 *seat = &run->seats[index];
		ce_unbind_v1 unbind;

		if (!seat->ce_bound)
		{
			continue;
		}
		memset(&unbind, 0, sizeof(unbind));
		init_header(&unbind.header, sizeof(unbind));
		unbind.engine_slot = seat->bind_engine_slot;
		unbind.client_generation = seat->client_generation;
		copy_string(unbind.run_nonce, sizeof(unbind.run_nonce), run->run_nonce);
		copy_string(unbind.seat_nonce, sizeof(unbind.seat_nonce), seat->seat_nonce);
		if (run->call(run->call_context, CE_UNBIND, &unbind,
			(intptr_t) sizeof(unbind)) == CE_RESULT_OK)
		{
			seat->ce_bound = 0;
			seat->bind_engine_slot = 0u;
			seat->client_generation = 0u;
		}
	}
	if (any_bound(run))
	{
		return SOL_EVIDENCE_CLEANUP_RETRY;
	}
	for (index = 0; index < SOL_EVIDENCE_RUN_SEATS_V1; ++index)
	{
		sol_evidence_run_seat_v1 *seat = &run->seats[index];

		if (!seat->client_present)
		{
			continue;
		}
		if (!remove)
		{
			return SOL_EVIDENCE_CLEANUP_RETRY;
		}
		remove(remove_context, index, seat->client_slot);
		seat->client_present = 0;
	}
	memset(run, 0, sizeof(*run));
	return SOL_EVIDENCE_CLEANUP_COMPLETE;
}
