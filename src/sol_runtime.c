#include "g_local.h"

#include "controller_evidence_protocol.h"
#include "controller_observation_protocol.h"
#include "sol_actual_command.h"
#include "sol_candidate_registry.h"
#include "sol_evidence_run.h"
#include "sol_ktx_adapter.h"
#include "sol_launch_coordinator.h"
#include "sol_runtime.h"
#include "sol_runtime_schedule.h"

#include <math.h>
#include <string.h>

typedef struct sol_runtime_v1
{
	sol_candidate_registry_v1 *candidates;
	sol_evidence_run_v1 *evidence;
	sol_launch_coordinator_v1 *launches;
	uint64_t diagnostic_frame_seq[SOL_KTX_CANDIDATE_COUNT_V1];
} sol_runtime_v1;

static sol_runtime_v1 sol;

static const sol_ktx_seat_identity_v1 *identity_for_index(size_t index)
{
	static const char *const plan_seats[SOL_KTX_EVIDENCE_SEAT_COUNT_V1] = {
		"1", "2", "3", "4", "5", "6", "7", "8"
	};

	return index < SOL_KTX_EVIDENCE_SEAT_COUNT_V1 ?
		sol_ktx_plan_identity_v1(plan_seats[index]) : NULL;
}

static void init_header(ce_payload_header_v1 *header, size_t size)
{
	header->protocol_version = CE_PROTOCOL_VERSION_V1;
	header->struct_size = (uint32_t) size;
}

static int lowercase_hex64(const char *value)
{
	size_t index;

	if (!value || strlen(value) != 64)
	{
		return 0;
	}
	for (index = 0; index < 64; ++index)
	{
		if (!((value[index] >= '0' && value[index] <= '9')
				|| (value[index] >= 'a' && value[index] <= 'f')))
		{
			return 0;
		}
	}
	return 1;
}

static int sha256_id(const char *value)
{
	return value && strlen(value) == 71 && !memcmp(value, "sha256:", 7)
		&& lowercase_hex64(value + 7);
}

static int read_arg(int index, char *output, size_t capacity)
{
	char value[256];
	size_t length;

	trap_CmdArgv(index, value, sizeof(value));
	length = strlen(value);
	if (length == 0 || length >= capacity)
	{
		return 0;
	}
	memcpy(output, value, length + 1);
	return 1;
}

static int read_sha256_hex_arg(int index, char output[CE_SHA256_HEX_CAP])
{
	return read_arg(index, output, CE_SHA256_HEX_CAP) && lowercase_hex64(output);
}

static int read_sha256_id_arg(int index, char *output, size_t capacity)
{
	return read_arg(index, output, capacity) && sha256_id(output);
}

static int evidence_extension_available(void)
{
	return HAVEEXTENSION(G_CONTROLLER_EVIDENCE_V1);
}

static int observation_extension_available(void)
{
	return HAVEEXTENSION(G_CONTROLLER_OBSERVATION_V1);
}

static intptr_t call_evidence(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	(void) context;
	return trap_ControllerEvidenceV1(operation, payload, payload_size);
}

static intptr_t call_observation(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	(void) context;
	return trap_ControllerObservationV1(operation, payload, payload_size);
}

static int ensure_runtime(void)
{
	if (!sol.evidence)
	{
		sol.evidence = sol_evidence_run_create_v1();
	}
	if (!sol.candidates)
	{
		sol.candidates = sol_candidate_registry_create_v1();
	}
	if (!sol.launches)
	{
		sol.launches = sol_launch_coordinator_create_v1();
	}
	return sol.evidence != NULL && sol.candidates != NULL && sol.launches != NULL;
}

static void reset_runtime_clients(void)
{
	sol_candidate_registry_destroy_v1(sol.candidates);
	sol.candidates = NULL;
	sol_launch_coordinator_destroy_v1(sol.launches);
	sol.launches = NULL;
	memset(sol.diagnostic_frame_seq, 0, sizeof(sol.diagnostic_frame_seq));
}

static int entry_for_client(const struct gedict_s *client, size_t *found_index,
	sol_candidate_entry_view_v1 *found_entry)
{
	size_t index;

	if (!client || !sol.candidates)
	{
		return 0;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_candidate_entry_view_v1 entry;

		if (!sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
			|| entry.stage < SOL_CANDIDATE_CLAIMED || entry.entity < 1
			|| entry.entity > MAX_CLIENTS || client != &g_edicts[entry.entity])
		{
			continue;
		}
		if (found_index)
		{
			*found_index = index;
		}
		if (found_entry)
		{
			*found_entry = entry;
		}
		return 1;
	}
	return 0;
}

int Sol_IsClient(const struct gedict_s *client)
{
	return entry_for_client(client, NULL, NULL);
}

static int claim_client(gedict_t *client)
{
	int entity;
	size_t claimed_index;

	if (!client || !client->isBot || !client->netname || !sol.candidates)
	{
		return 0;
	}
	entity = NUM_FOR_EDICT(client);
	if (entity < 1 || entity > MAX_CLIENTS ||
		!sol_candidate_registry_claim_v1(sol.candidates, client->netname,
			entity, &claimed_index))
	{
		return 0;
	}
	client->blocked = 0;
	G_cprint("[sol-slice/v1] claimed seat=%u name=%s slot=%d\n",
		(unsigned) claimed_index + 1u, client->netname, entity);
	return 1;
}

int Sol_ClientConnectedEvent(struct gedict_s *client)
{
	return Sol_IsClient(client) || claim_client(client);
}

int Sol_ClientEntersEvent(struct gedict_s *client)
{
	int claimed = Sol_IsClient(client) || claim_client(client);

	if (claimed)
	{
		client->blocked = 0;
	}
	return claimed;
}

void Sol_ClientDisconnectedEvent(struct gedict_s *client)
{
	sol_candidate_entry_view_v1 entry;
	size_t index = SOL_KTX_CANDIDATE_COUNT_V1;
	int entity;
	int registry_owned;

	if (!client)
	{
		return;
	}
	entity = NUM_FOR_EDICT(client);
	registry_owned = entry_for_client(client, &index, &entry);
	if (!registry_owned && (!sol.evidence || entity < 1 || entity > MAX_CLIENTS
		|| !sol_evidence_run_find_client_v1(sol.evidence, (uint32_t) entity,
			&index)))
	{
		return;
	}
	if (sol.evidence)
	{
		sol_evidence_run_note_disconnect_v1(sol.evidence, index);
	}
	if (registry_owned)
	{
		G_cprint("[sol-slice/v1] disconnect seat=%u slot=%d generation=%u\n",
			(unsigned) index + 1u, entry.entity, entry.client_generation);
		sol_candidate_registry_release_v1(sol.candidates, index);
	}
	else
	{
		FrogbotsForgetBotByEntity(entity);
	}
}

static void remove_client_after_unbind(void *context, size_t index,
	uint32_t engine_slot)
{
	const sol_ktx_seat_identity_v1 *identity;
	sol_candidate_entry_view_v1 entry;

	(void) context;
	identity = identity_for_index(index);
	if (!identity)
	{
		return;
	}
	if (identity->role == SOL_KTX_SEAT_CONTROL_V1)
	{
		FrogbotsRemoveBotByEntity((int) engine_slot);
	}
	else if (engine_slot >= 1u && engine_slot <= MAX_CLIENTS
		&& g_edicts[engine_slot].isBot)
	{
		trap_RemoveBot((int) engine_slot);
	}
	if (identity->role == SOL_KTX_SEAT_CANDIDATE_V1 && sol.candidates &&
		sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
		&& entry.stage != SOL_CANDIDATE_EMPTY)
	{
		sol_candidate_registry_release_v1(sol.candidates, index);
	}
}

void Sol_ServerStartFrame(void)
{
	sol_evidence_cleanup_result_v1 result;
	sol_runtime_schedule_decision_v1 schedule;

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_SERVER_FRAME_V1,
		sol_evidence_run_active_v1(sol.evidence),
		sol_evidence_run_emissions_open_v1(sol.evidence), sol.candidates != NULL,
		sol_evidence_run_cleanup_pending_v1(sol.evidence));
	if (!schedule.run_cleanup)
	{
		return;
	}
	result = sol_evidence_run_server_cleanup_v1(sol.evidence,
		remove_client_after_unbind, NULL);
	if (result == SOL_EVIDENCE_CLEANUP_COMPLETE)
	{
		G_cprint("[sol-slice/v1] safe server-frame cleanup complete\n");
		reset_runtime_clients();
	}
	else if (result == SOL_EVIDENCE_CLEANUP_RETRY)
	{
		G_cprint("[sol-slice/v1] safe server-frame cleanup remains fail-closed\n");
	}
}

static uint8_t command_msec(void)
{
	int msec = (int) (g_globalvars.frametime * 1000.0f + 0.5f);

	return (uint8_t) bound(1, msec, 255);
}

static int frame_candidate_healthy(void *context, size_t index, int entity,
	uint32_t client_generation)
{
	(void) context;
	(void) index;
	(void) client_generation;
	return entity >= 1 && entity <= MAX_CLIENTS && g_edicts[entity].isBot
		&& Sol_IsClient(&g_edicts[entity]);
}

static int frame_submit_decision(void *context, size_t index, int entity,
	uint32_t client_generation, const uint8_t *action_response,
	size_t action_response_length, const uint8_t *decision_trace,
	size_t decision_trace_length)
{
	sol_evidence_run_v1 *evidence = context;
	uint32_t bound_slot = 0u;
	uint32_t bound_generation = 0u;

	return evidence &&
		sol_evidence_run_binding_v1(evidence, index, &bound_slot,
			&bound_generation) && bound_slot == (uint32_t) entity &&
		bound_generation == client_generation &&
		sol_evidence_run_submit_decision_v1(evidence, index,
			action_response, action_response_length, decision_trace,
			decision_trace_length);
}

static int frame_write_commands(void *context,
	const sol_candidate_command_batch_item_v1 *items, size_t count,
	sol_candidate_command_batch_result_v1 *results)
{
	sol_actual_command_input_v1 inputs[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_actual_command_input_v1 neutral_inputs[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_actual_command_batch_result_v1 actual_results[
		SOL_KTX_CANDIDATE_COUNT_V1];
	int accepted;
	size_t item;

	(void) context;
	if (!items || !results || !count || count > SOL_KTX_CANDIDATE_COUNT_V1)
	{
		return 0;
	}
	memset(inputs, 0, sizeof(inputs));
	memset(neutral_inputs, 0, sizeof(neutral_inputs));
	memset(actual_results, 0, sizeof(actual_results));
	for (item = 0u; item < count; ++item)
	{
		inputs[item].engine_slot = items[item].entity;
		inputs[item].msec = items[item].command.msec;
		memcpy(inputs[item].angles, items[item].command.angles,
			sizeof(inputs[item].angles));
		inputs[item].forwardmove = items[item].command.forwardmove;
		inputs[item].sidemove = items[item].command.sidemove;
		inputs[item].upmove = items[item].command.upmove;
		inputs[item].buttons = items[item].command.buttons;
		inputs[item].impulse = items[item].command.impulse;
		neutral_inputs[item].engine_slot = items[item].entity;
		neutral_inputs[item].msec = items[item].neutral_command.msec;
		memcpy(neutral_inputs[item].angles,
			items[item].neutral_command.angles,
			sizeof(neutral_inputs[item].angles));
		neutral_inputs[item].forwardmove =
			items[item].neutral_command.forwardmove;
		neutral_inputs[item].sidemove = items[item].neutral_command.sidemove;
		neutral_inputs[item].upmove = items[item].neutral_command.upmove;
		neutral_inputs[item].buttons = items[item].neutral_command.buttons;
		neutral_inputs[item].impulse = items[item].neutral_command.impulse;
	}
	accepted = trap_SetSolBotCMDBatch(inputs, neutral_inputs, count,
		actual_results);
	for (item = 0u; item < count; ++item)
	{
		results[item].request_status =
			actual_results[item].request_status ==
				SOL_ACTUAL_COMMAND_REQUEST_ACCEPTED ?
				SOL_CANDIDATE_REQUEST_ACCEPTED :
				(actual_results[item].request_status ==
					SOL_ACTUAL_COMMAND_REQUEST_REJECTED ?
					SOL_CANDIDATE_REQUEST_REJECTED :
					SOL_CANDIDATE_REQUEST_NOT_RUN);
		results[item].emitted = actual_results[item].emitted;
	}
	return accepted;
}

void Sol_StartFrame(void)
{
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_runtime_schedule_decision_v1 schedule;
	sol_candidate_frame_ops_v1 ops = {
		.context = sol.evidence,
		.healthy = frame_candidate_healthy,
		.decision = frame_submit_decision,
		.commands = frame_write_commands
	};
	uint8_t msec;
	uint32_t dt_us;
	size_t index;

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_BOT_FRAME_V1,
		sol_evidence_run_active_v1(sol.evidence),
		sol_evidence_run_emissions_open_v1(sol.evidence), sol.candidates != NULL,
		sol_evidence_run_cleanup_pending_v1(sol.evidence));
	if (!schedule.run_candidates)
	{
		return;
	}
	msec = command_msec();
	dt_us = (uint32_t) msec * 1000u;
	sol_candidate_registry_run_frame_v1(sol.candidates, msec, dt_us, &ops, results);
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_candidate_entry_view_v1 entry;

		if (!sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
			|| entry.stage != SOL_CANDIDATE_BOUND)
		{
			continue;
		}
		G_cprint("[sol-slice/v1] seat=%u frame=%llu observation=%d bytes=%lu "
			"policy=%d action=%lu trace=%lu proof=%d request=%d "
			"cancelled=%d emitted=%d\n",
			(unsigned) index + 1u,
			(unsigned long long) sol.diagnostic_frame_seq[index]++,
			(int) results[index].observation_status,
			(unsigned long) results[index].batch_length,
			(int) results[index].brain_status,
			(unsigned long) results[index].action_length,
			(unsigned long) results[index].trace_length,
			(int) results[index].decision_status,
			(int) results[index].request_status,
			results[index].frame_cancelled,
			results[index].emitted);
		if (results[index].frame_cancelled ||
			(results[index].emitted &&
				(results[index].observation_status == SOL_OBSERVATION_INVALID
				|| results[index].brain_status < SOL_BRAIN_NEUTRAL
				|| (results[index].observation_status == SOL_OBSERVATION_READY
					&& results[index].decision_status !=
						SOL_CANDIDATE_DECISION_SUBMITTED))))
		{
			sol_evidence_run_fail_stop_v1(sol.evidence);
		}
	}
}

void Sol_MatchTimelineBegin(void)
{
	const char *run_nonce = sol_evidence_run_nonce_v1(sol.evidence);

	if (!run_nonce || !sol_evidence_run_matches_v1(sol.evidence, run_nonce,
			"ktx-match/v1"))
	{
		return;
	}
	if (!sol_evidence_run_match_timeline_begin_v1(sol.evidence))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
	}
}

sol_actual_command_route_v1 Sol_ActualCommandLookup(uint32_t engine_slot,
	uint32_t *client_generation)
{
	sol_evidence_command_route_v1 result;

	if (client_generation)
	{
		*client_generation = 0u;
	}
	result = sol_evidence_run_command_route_v1(sol.evidence, engine_slot,
		client_generation);
	if (result == SOL_EVIDENCE_COMMAND_BOUND)
	{
		return SOL_ACTUAL_COMMAND_BOUND;
	}
	if (result == SOL_EVIDENCE_COMMAND_AMBIGUOUS)
	{
		return SOL_ACTUAL_COMMAND_AMBIGUOUS;
	}
	if (result == SOL_EVIDENCE_COMMAND_QUARANTINED)
	{
		return SOL_ACTUAL_COMMAND_QUARANTINED;
	}
	return SOL_ACTUAL_COMMAND_UNBOUND;
}

void Sol_ActualCommandFailStop(void)
{
	sol_evidence_run_fail_stop_v1(sol.evidence);
}

int Sol_CommandBypassesBotGates(const char *command)
{
	return command && (!strcmp(command, "evidencebind") || !strcmp(command, "addsol")
		|| !strcmp(command, "evidenceclose") || !strcmp(command, "removeall"));
}

int Sol_BotReadyAllowed(void)
{
	const char *run_nonce;

	if (!sol.evidence || !sol_evidence_run_active_v1(sol.evidence))
	{
		return 1;
	}
	run_nonce = sol_evidence_run_nonce_v1(sol.evidence);
	if (!run_nonce)
	{
		return 0;
	}
	if (sol_evidence_run_matches_v1(sol.evidence, run_nonce,
			"diagnostic-client-lifecycle/v1"))
	{
		return 1;
	}
	if (!sol_evidence_run_matches_v1(sol.evidence, run_nonce, "ktx-match/v1"))
	{
		return 0;
	}
	return !sol_evidence_run_cleanup_pending_v1(sol.evidence)
		&& sol_evidence_run_emissions_open_v1(sol.evidence)
		&& sol_launch_coordinator_all_complete_v1(sol.launches);
}

void Sol_EvidenceBind_f(void)
{
	sol_launch_metadata_v1 metadata;
	const sol_ktx_seat_identity_v1 *identity;
	char plan_seat[CE_SEAT_ID_CAP];
	char run_nonce[CE_RUN_NONCE_CAP];
	char epoch_kind[CE_EPOCH_KIND_CAP];
	size_t index;
	int active;

	if (!evidence_extension_available())
	{
		G_sprint(self, PRINT_HIGH,
			"ControllerEvidenceV1 is unavailable.\n");
		return;
	}
	if (trap_CmdArgc() != 13)
	{
		G_sprint(self, PRINT_HIGH, "Usage: /botcmd evidencebind <seat> <run_nonce> "
			"<epoch_kind> <seat_nonce> <profile_id> <display_version> "
			"<controller_digest> <build_id> <config_sha256> <treatment_digest> "
			"<writer_id>\n");
		return;
	}
	memset(&metadata, 0, sizeof(metadata));
	if (!read_arg(2, plan_seat, sizeof(plan_seat)))
	{
		G_sprint(self, PRINT_HIGH, "Invalid SOL plan seat.\n");
		return;
	}
	identity = sol_ktx_plan_identity_v1(plan_seat);
	if (!identity)
	{
		G_sprint(self, PRINT_HIGH, "SOL evidencebind accepts lifecycle seats 1..8.\n");
		return;
	}
	index = identity->ordinal - 1u;
	if (identity->role == SOL_KTX_SEAT_CANDIDATE_V1
		&& !observation_extension_available())
	{
		G_sprint(self, PRINT_HIGH,
			"ControllerObservationV1 is unavailable for candidate seats.\n");
		return;
	}
	if (!read_sha256_hex_arg(3, run_nonce)
		|| !read_arg(4, epoch_kind, sizeof(epoch_kind))
		|| (strcmp(epoch_kind, "diagnostic-client-lifecycle/v1")
			&& strcmp(epoch_kind, "ktx-match/v1"))
		|| !read_sha256_hex_arg(5, metadata.seat_nonce)
		|| !read_arg(6, metadata.controller_id, sizeof(metadata.controller_id))
		|| !read_arg(7, metadata.controller_version,
			sizeof(metadata.controller_version))
		|| !read_sha256_id_arg(8, metadata.controller_digest,
			sizeof(metadata.controller_digest))
		|| !read_sha256_id_arg(9, metadata.build_id, sizeof(metadata.build_id))
		|| !read_sha256_hex_arg(10, metadata.config_sha256)
		|| !read_sha256_id_arg(11, metadata.treatment_digest,
			sizeof(metadata.treatment_digest))
		|| !read_arg(12, metadata.writer_id, sizeof(metadata.writer_id))
		|| !ensure_runtime())
	{
		G_sprint(self, PRINT_HIGH, "Invalid sealed SOL evidence arguments.\n");
		return;
	}
	active = sol_evidence_run_active_v1(sol.evidence);
	if ((active && (!sol_evidence_run_matches_v1(sol.evidence, run_nonce,
			epoch_kind) || sol_evidence_run_cleanup_pending_v1(sol.evidence)))
		|| sol_launch_coordinator_pending_v1(sol.launches, NULL) != NULL
		|| sol_launch_coordinator_seat_v1(sol.launches, index) != NULL
		|| sol_evidence_run_seat_configured_v1(sol.evidence, index))
	{
		G_sprint(self, PRINT_HIGH,
			"SOL evidence seat is duplicate, closing, or belongs to another run.\n");
		return;
	}
	if (!active && !sol_evidence_run_begin_v1(sol.evidence, run_nonce, epoch_kind,
		call_evidence, NULL))
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence epoch begin rejected.\n");
		return;
	}
	if (!sol_evidence_run_configure_seat_v1(sol.evidence, index,
		metadata.seat_nonce))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL evidence seat configuration failed closed.\n");
		return;
	}
	if (identity->role == SOL_KTX_SEAT_CANDIDATE_V1
		&& !sol_candidate_registry_set_pending_v1(sol.candidates, index))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL candidate registry rejected the pending seat.\n");
		return;
	}
	if (!sol_launch_coordinator_configure_v1(sol.launches, index, &metadata))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL launch coordinator failed closed.\n");
		return;
	}
	G_sprint(self, PRINT_HIGH, "SOL plan seat %s (%s/%s) pending.\n",
		identity->plan_seat, identity->evidence_seat, identity->player_name);
}

static int pending_candidate_index(size_t *pending_index)
{
	sol_candidate_entry_view_v1 entry;
	const sol_ktx_seat_identity_v1 *identity;
	size_t index;

	if (!sol.candidates || !sol.evidence || !sol.launches
		|| !sol_launch_coordinator_pending_v1(sol.launches, &index))
	{
		return 0;
	}
	identity = identity_for_index(index);
	if (!identity || identity->role != SOL_KTX_SEAT_CANDIDATE_V1
		|| !sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
		|| entry.stage != SOL_CANDIDATE_PENDING
		|| !sol_evidence_run_seat_configured_v1(sol.evidence, index))
	{
		return 0;
	}
	*pending_index = index;
	return 1;
}

static int configured_stuck_replan_ms(uint32_t *output)
{
	float value = cvar("k_sol_stuck_replan_ms");
	uint32_t parsed;

	if (!output || !isfinite((double) value)
		|| value < (float) SOL_BRAIN_STUCK_REPLAN_MIN_MS_V1
		|| value > (float) SOL_BRAIN_STUCK_REPLAN_MAX_MS_V1)
	{
		return 0;
	}
	parsed = (uint32_t) value;
	if ((float) parsed != value)
	{
		return 0;
	}
	*output = parsed;
	return 1;
}

static int bind_initialized_client(size_t index, int entity,
	const char *initialized_name, const char *initialized_team)
{
	const sol_launch_metadata_v1 *metadata;
	const sol_ktx_seat_identity_v1 *identity;
	sol_evidence_bind_record_result_v1 recorded = SOL_EVIDENCE_BIND_REJECTED;
	sol_candidate_entry_view_v1 entry;
	const cov_profile_v1 *profile = NULL;
	const char *run_nonce;
	ce_bind_v1 bind;
	uint32_t stuck_replan_ms = 0u;
	size_t pending_index;
	int result;

	metadata = sol_launch_coordinator_pending_v1(sol.launches, &pending_index);
	identity = identity_for_index(index);
	if (!metadata || pending_index != index || !identity || entity < 1
		|| entity > MAX_CLIENTS || !sol_evidence_run_active_v1(sol.evidence)
		|| sol_evidence_run_cleanup_pending_v1(sol.evidence)
		|| !sol_evidence_run_record_client_v1(sol.evidence, index,
			(uint32_t) entity))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		return 0;
	}
	run_nonce = sol_evidence_run_nonce_v1(sol.evidence);
	memset(&bind, 0, sizeof(bind));
	init_header(&bind.header, sizeof(bind));
	bind.engine_slot = (uint32_t) entity;
	strlcpy(bind.run_nonce, run_nonce, sizeof(bind.run_nonce));
	strlcpy(bind.seat_id, identity->evidence_seat, sizeof(bind.seat_id));
	strlcpy(bind.seat_nonce, metadata->seat_nonce, sizeof(bind.seat_nonce));
	strlcpy(bind.controller_id, metadata->controller_id,
		sizeof(bind.controller_id));
	strlcpy(bind.controller_version, metadata->controller_version,
		sizeof(bind.controller_version));
	strlcpy(bind.controller_digest, metadata->controller_digest,
		sizeof(bind.controller_digest));
	strlcpy(bind.build_id, metadata->build_id, sizeof(bind.build_id));
	strlcpy(bind.config_sha256, metadata->config_sha256,
		sizeof(bind.config_sha256));
	strlcpy(bind.treatment_digest, metadata->treatment_digest,
		sizeof(bind.treatment_digest));
	strlcpy(bind.writer_id, metadata->writer_id, sizeof(bind.writer_id));
	result = trap_ControllerEvidenceV1(CE_BIND, &bind, sizeof(bind));
	if (result == CE_RESULT_OK)
	{
		/* Retain every successful engine route before trusting any identity. */
		recorded = sol_evidence_run_record_bind_v1(sol.evidence, index,
			(uint32_t) entity, bind.client_generation);
	}
	if (result != CE_RESULT_OK || recorded != SOL_EVIDENCE_BIND_ACCEPTED
		|| !bind.client_generation || !initialized_name || !initialized_team
		|| strcmp(initialized_name, identity->player_name)
		|| strcmp(initialized_team, identity->team)
		|| strcmp(bind.observed_player_name, identity->player_name)
		|| strcmp(bind.observed_team, identity->team))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH,
			"SOL evidence bind failed closed (%d, name=%s, team=%s).\n",
			result, bind.observed_player_name, bind.observed_team);
		return 0;
	}
	if (identity->role == SOL_KTX_SEAT_CANDIDATE_V1)
	{
		if (!configured_stuck_replan_ms(&stuck_replan_ms)
			|| !sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
			|| entry.stage != SOL_CANDIDATE_CLAIMED || entry.entity != entity
			|| !sol_candidate_registry_bind_v1(sol.candidates, index,
				bind.client_generation, stuck_replan_ms,
				call_observation, NULL))
		{
			sol_evidence_run_fail_stop_v1(sol.evidence);
			G_sprint(self, PRINT_HIGH,
				"SOL candidate lifecycle/profile bind failed closed for slot %d.\n",
				entity);
			return 0;
		}
		profile = sol_candidate_registry_profile_v1(sol.candidates, index);
	}
	if (!sol_launch_coordinator_complete_v1(sol.launches, index))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		return 0;
	}
	G_sprint(self, PRINT_HIGH,
		"SOL plan seat %s (%s) bound to %s/%s generation %u COV batch cap %u.\n",
		identity->plan_seat, identity->evidence_seat, identity->player_name,
		identity->team, bind.client_generation,
		profile ? profile->max_batch_bytes : 0u);
	return 1;
}

int Sol_StockPendingBotName(int skill_level, const char *team,
	char *output, size_t capacity)
{
	const sol_ktx_seat_identity_v1 *identity;
	size_t index;
	size_t length;

	if (!output || !capacity || !sol.launches || !sol.evidence
		|| !sol_evidence_run_active_v1(sol.evidence)
		|| sol_evidence_run_cleanup_pending_v1(sol.evidence)
		|| !sol_launch_coordinator_pending_v1(sol.launches, &index)
		|| !sol_ktx_control_selector_v1(skill_level, team))
	{
		return 0;
	}
	identity = identity_for_index(index);
	if (!identity || identity->role != SOL_KTX_SEAT_CONTROL_V1)
	{
		return 0;
	}
	length = strlen(identity->player_name) + 1u;
	if (length > capacity)
	{
		return 0;
	}
	memcpy(output, identity->player_name, length);
	return 1;
}

int Sol_StockBotInitialized(int entity, int skill_level, const char *team,
	const char *name)
{
	const sol_ktx_seat_identity_v1 *identity;
	size_t index;

	if (!sol.launches || !sol_launch_coordinator_pending_v1(sol.launches, &index))
	{
		return 0;
	}
	identity = identity_for_index(index);
	if (!identity || identity->role != SOL_KTX_SEAT_CONTROL_V1
		|| !sol_ktx_control_selector_v1(skill_level, team))
	{
		return 0;
	}
	return bind_initialized_client(index, entity, name, team);
}

void Sol_Add_f(void)
{
	char seat[32];
	char team[32];
	const sol_ktx_seat_identity_v1 *identity;
	size_t index;
	int entity;

	if (trap_CmdArgc() != 4 || !read_arg(2, seat, sizeof(seat))
		|| !read_arg(3, team, sizeof(team)) || !sol_ktx_add_shape_v1(seat, team))
	{
		G_sprint(self, PRINT_HIGH, "Usage: /botcmd addsol 20 red\n");
		return;
	}
	if (!sol.evidence || !sol_evidence_run_active_v1(sol.evidence)
		|| sol_evidence_run_cleanup_pending_v1(sol.evidence)
		|| !pending_candidate_index(&index))
	{
		G_sprint(self, PRINT_HIGH,
			"Bind exactly one pending SOL candidate seat before addsol.\n");
		return;
	}
	identity = identity_for_index(index);
	if (!identity || !sol_candidate_registry_expect_client_v1(sol.candidates, index))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL candidate claim window failed closed.\n");
		return;
	}
	entity = (int) trap_AddBot(identity->player_name, 4, 4, "base");
	if (!entity)
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL candidate add failed closed.\n");
		return;
	}
	g_edicts[entity].blocked = 0;
	trap_SetBotUserInfo(entity, "team", identity->team, 0);
	if (!bind_initialized_client(index, entity, identity->player_name,
		identity->team))
	{
		G_sprint(self, PRINT_HIGH, "SOL candidate bind failed closed.\n");
	}
}

void Sol_EvidenceClose_f(void)
{
	char run_nonce[CE_RUN_NONCE_CAP];
	const char *active_nonce;

	if (trap_CmdArgc() != 3 || !read_arg(2, run_nonce, sizeof(run_nonce))
		|| !lowercase_hex64(run_nonce))
	{
		G_sprint(self, PRINT_HIGH, "Usage: /botcmd evidenceclose <run_nonce>\n");
		return;
	}
	active_nonce = sol.evidence ? sol_evidence_run_nonce_v1(sol.evidence) : NULL;
	if (!active_nonce || strcmp(run_nonce, active_nonce))
	{
		G_sprint(self, PRINT_HIGH, "SOL run nonce does not match the active run.\n");
		return;
	}
	if (sol_evidence_run_cleanup_pending_v1(sol.evidence))
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence cleanup already pending.\n");
		return;
	}
	sol_evidence_run_request_close_v1(sol.evidence);
	G_sprint(self, PRINT_HIGH,
		"SOL evidence close scheduled for the next safe server frame.\n");
}

int Sol_RemoveAll(void)
{
	if (sol.evidence && sol_evidence_run_active_v1(sol.evidence))
	{
		sol_evidence_run_request_close_v1(sol.evidence);
		return 1;
	}
	reset_runtime_clients();
	return 0;
}
