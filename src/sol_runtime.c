#include "g_local.h"

#include "controller_evidence_protocol.h"
#include "controller_observation_protocol.h"
#include "sol_candidate_registry.h"
#include "sol_evidence_run.h"
#include "sol_ktx_adapter.h"
#include "sol_runtime.h"
#include "sol_runtime_schedule.h"

#include <string.h>

#define SOL_TEAM "red"

typedef struct sol_runtime_seat_v1
{
	int configured;
	char seat_nonce[CE_SEAT_NONCE_CAP];
	char controller_id[CE_CONTROLLER_ID_CAP];
	char controller_version[CE_CONTROLLER_VERSION_CAP];
	char controller_digest[CE_SHA256_ID_CAP];
	char build_id[CE_BUILD_ID_CAP];
	char config_sha256[CE_SHA256_HEX_CAP];
	char treatment_digest[CE_SHA256_ID_CAP];
	char writer_id[CE_WRITER_ID_CAP];
	uint64_t diagnostic_frame_seq;
} sol_runtime_seat_v1;

typedef struct sol_runtime_v1
{
	sol_candidate_registry_v1 *candidates;
	sol_evidence_run_v1 *evidence;
	sol_runtime_seat_v1 seats[SOL_KTX_CANDIDATE_COUNT_V1];
} sol_runtime_v1;

static sol_runtime_v1 sol;

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
	return sol.evidence != NULL && sol.candidates != NULL;
}

static void reset_runtime_candidates(void)
{
	sol_candidate_registry_destroy_v1(sol.candidates);
	sol.candidates = NULL;
	memset(sol.seats, 0, sizeof(sol.seats));
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
	if (!sol.evidence || !sol_evidence_run_active_v1(sol.evidence))
	{
		memset(&sol.seats[index], 0, sizeof(sol.seats[index]));
	}
}

static void remove_candidate_after_unbind(void *context, size_t index,
	uint32_t engine_slot)
{
	sol_candidate_entry_view_v1 entry;

	(void) context;
	if (engine_slot >= 1u && engine_slot <= MAX_CLIENTS
		&& g_edicts[engine_slot].isBot)
	{
		trap_RemoveBot((int) engine_slot);
	}
	if (sol.candidates &&
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
		sol_evidence_run_active_v1(sol.evidence), sol.candidates != NULL,
		sol_evidence_run_cleanup_pending_v1(sol.evidence));
	if (!schedule.run_cleanup)
	{
		return;
	}
	result = sol_evidence_run_server_cleanup_v1(sol.evidence,
		remove_candidate_after_unbind, NULL);
	if (result == SOL_EVIDENCE_CLEANUP_COMPLETE)
	{
		G_cprint("[sol-slice/v1] safe server-frame cleanup complete\n");
		reset_runtime_candidates();
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

static int frame_write_evidence(void *context, size_t index, int entity,
	uint32_t client_generation,
	const uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE])
{
	ce_frame_request_v1 request;

	(void) context;
	(void) index;
	memset(&request, 0, sizeof(request));
	init_header(&request.header, sizeof(request));
	request.engine_slot = (uint32_t) entity;
	request.client_generation = client_generation;
	memcpy(request.requested_command.bytes, command_wire,
		sizeof(request.requested_command.bytes));
	return trap_ControllerEvidenceV1(CE_FRAME_REQUEST, &request, sizeof(request));
}

static void frame_write_command(void *context, size_t index, int entity,
	const sol_ktx_command_v1 *command)
{
	(void) context;
	(void) index;
	trap_SetBotCMD(entity, command->msec, command->angles[0], command->angles[1],
		command->angles[2], command->forwardmove, command->sidemove,
		command->upmove, command->buttons, command->impulse);
}

void Sol_StartFrame(void)
{
	sol_candidate_frame_result_v1 results[SOL_KTX_CANDIDATE_COUNT_V1];
	sol_runtime_schedule_decision_v1 schedule;
	sol_candidate_frame_ops_v1 ops = {
		NULL, frame_candidate_healthy, frame_write_evidence, frame_write_command
	};
	uint8_t msec;
	uint32_t dt_us;
	size_t index;

	schedule = sol_runtime_schedule_decide_v1(SOL_RUNTIME_BOT_FRAME_V1,
		sol_evidence_run_active_v1(sol.evidence), sol.candidates != NULL,
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
			"policy=neutral evidence=%d emitted=%d\n",
			(unsigned) index + 1u,
			(unsigned long long) sol.seats[index].diagnostic_frame_seq++,
			(int) results[index].observation_status,
			(unsigned long) results[index].batch_length,
			results[index].evidence_result, results[index].emitted);
		if (results[index].emitted &&
			(results[index].observation_status == SOL_OBSERVATION_INVALID
				|| results[index].evidence_result != CE_RESULT_OK))
		{
			sol_evidence_run_fail_stop_v1(sol.evidence);
		}
	}
}

int Sol_CommandBypassesBotGates(const char *command)
{
	return command && (!strcmp(command, "evidencebind") || !strcmp(command, "addsol")
		|| !strcmp(command, "evidenceclose") || !strcmp(command, "removeall"));
}

void Sol_EvidenceBind_f(void)
{
	sol_runtime_seat_v1 pending;
	const sol_ktx_seat_identity_v1 *identity;
	sol_candidate_entry_view_v1 entry;
	char plan_seat[CE_SEAT_ID_CAP];
	char run_nonce[CE_RUN_NONCE_CAP];
	char epoch_kind[CE_EPOCH_KIND_CAP];
	size_t index;
	int active;

	if (!evidence_extension_available() || !observation_extension_available())
	{
		G_sprint(self, PRINT_HIGH,
			"ControllerEvidenceV1 or ControllerObservationV1 is unavailable.\n");
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
	memset(&pending, 0, sizeof(pending));
	if (!read_arg(2, plan_seat, sizeof(plan_seat)))
	{
		G_sprint(self, PRINT_HIGH, "Invalid SOL plan seat.\n");
		return;
	}
	identity = sol_ktx_plan_identity_v1(plan_seat);
	if (!identity)
	{
		G_sprint(self, PRINT_HIGH,
			"SOL evidencebind accepts candidate seats 1..4 only; control seats "
			"require the separate legacy evidence binder.\n");
		return;
	}
	index = identity->ordinal - 1u;
	if (!read_sha256_hex_arg(3, run_nonce)
		|| !read_arg(4, epoch_kind, sizeof(epoch_kind))
		|| (strcmp(epoch_kind, "diagnostic-client-lifecycle/v1")
			&& strcmp(epoch_kind, "ktx-match/v1"))
		|| !read_sha256_hex_arg(5, pending.seat_nonce)
		|| !read_arg(6, pending.controller_id, sizeof(pending.controller_id))
		|| !read_arg(7, pending.controller_version,
			sizeof(pending.controller_version))
		|| !read_sha256_id_arg(8, pending.controller_digest,
			sizeof(pending.controller_digest))
		|| !read_sha256_id_arg(9, pending.build_id, sizeof(pending.build_id))
		|| !read_sha256_hex_arg(10, pending.config_sha256)
		|| !read_sha256_id_arg(11, pending.treatment_digest,
			sizeof(pending.treatment_digest))
		|| !read_arg(12, pending.writer_id, sizeof(pending.writer_id))
		|| !ensure_runtime())
	{
		G_sprint(self, PRINT_HIGH, "Invalid sealed SOL evidence arguments.\n");
		return;
	}
	active = sol_evidence_run_active_v1(sol.evidence);
	if ((active && (!sol_evidence_run_matches_v1(sol.evidence, run_nonce,
			epoch_kind) || sol_evidence_run_cleanup_pending_v1(sol.evidence)))
		|| !sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
		|| entry.stage != SOL_CANDIDATE_EMPTY || sol.seats[index].configured
		|| sol_evidence_run_seat_configured_v1(sol.evidence, index))
	{
		G_sprint(self, PRINT_HIGH,
			"SOL evidence seat is duplicate, closing, or belongs to another run.\n");
		return;
	}
	if (!sol_candidate_registry_set_pending_v1(sol.candidates, index))
	{
		G_sprint(self, PRINT_HIGH, "SOL candidate registry rejected the pending seat.\n");
		return;
	}
	if (!active && !sol_evidence_run_begin_v1(sol.evidence, run_nonce, epoch_kind,
		call_evidence, NULL))
	{
		sol_candidate_registry_release_v1(sol.candidates, index);
		G_sprint(self, PRINT_HIGH, "SOL evidence epoch begin rejected.\n");
		return;
	}
	if (!sol_evidence_run_configure_seat_v1(sol.evidence, index,
		pending.seat_nonce))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL evidence seat configuration failed closed.\n");
		return;
	}
	pending.configured = 1;
	sol.seats[index] = pending;
	G_sprint(self, PRINT_HIGH, "SOL plan seat %s (%s/%s) pending.\n",
		identity->plan_seat, identity->evidence_seat, identity->player_name);
}

static int pending_candidate_index(size_t *pending_index)
{
	size_t found = SOL_KTX_CANDIDATE_COUNT_V1;
	size_t index;

	if (!sol.candidates || !sol.evidence)
	{
		return 0;
	}
	for (index = 0; index < SOL_KTX_CANDIDATE_COUNT_V1; ++index)
	{
		sol_candidate_entry_view_v1 entry;

		if (sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
			&& entry.stage == SOL_CANDIDATE_PENDING && sol.seats[index].configured
			&& sol_evidence_run_seat_configured_v1(sol.evidence, index))
		{
			if (found != SOL_KTX_CANDIDATE_COUNT_V1)
			{
				return 0;
			}
			found = index;
		}
	}
	if (found == SOL_KTX_CANDIDATE_COUNT_V1)
	{
		return 0;
	}
	*pending_index = found;
	return 1;
}

void Sol_Add_f(void)
{
	char seat[32];
	char team[32];
	ce_bind_v1 bind;
	const cov_profile_v1 *profile;
	const sol_ktx_seat_identity_v1 *identity;
	sol_candidate_entry_view_v1 entry;
	const char *run_nonce;
	size_t index;
	int entity;
	int result;

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
	identity = sol_ktx_plan_identity_v1((const char *[]) { "1", "2", "3", "4" }[index]);
	if (!identity || !sol_candidate_registry_expect_client_v1(sol.candidates, index))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL candidate claim window failed closed.\n");
		return;
	}
	entity = (int) trap_AddBot(identity->player_name, 4, 4, "base");
	if (entity > 0 && !sol_evidence_run_record_client_v1(sol.evidence, index,
		(uint32_t) entity))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL client route recording failed closed.\n");
		return;
	}
	if (!entity || !sol_candidate_registry_entry_v1(sol.candidates, index, &entry)
		|| entry.stage != SOL_CANDIDATE_CLAIMED || entry.entity != entity)
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL client lifecycle claim failed closed.\n");
		return;
	}
	g_edicts[entity].blocked = 0;
	trap_SetBotUserInfo(entity, "team", SOL_TEAM, 0);
	run_nonce = sol_evidence_run_nonce_v1(sol.evidence);

	memset(&bind, 0, sizeof(bind));
	init_header(&bind.header, sizeof(bind));
	bind.engine_slot = (uint32_t) entity;
	strlcpy(bind.run_nonce, run_nonce, sizeof(bind.run_nonce));
	strlcpy(bind.seat_id, identity->evidence_seat, sizeof(bind.seat_id));
	strlcpy(bind.seat_nonce, sol.seats[index].seat_nonce, sizeof(bind.seat_nonce));
	strlcpy(bind.controller_id, sol.seats[index].controller_id,
		sizeof(bind.controller_id));
	strlcpy(bind.controller_version, sol.seats[index].controller_version,
		sizeof(bind.controller_version));
	strlcpy(bind.controller_digest, sol.seats[index].controller_digest,
		sizeof(bind.controller_digest));
	strlcpy(bind.build_id, sol.seats[index].build_id, sizeof(bind.build_id));
	strlcpy(bind.config_sha256, sol.seats[index].config_sha256,
		sizeof(bind.config_sha256));
	strlcpy(bind.treatment_digest, sol.seats[index].treatment_digest,
		sizeof(bind.treatment_digest));
	strlcpy(bind.writer_id, sol.seats[index].writer_id, sizeof(bind.writer_id));
	result = trap_ControllerEvidenceV1(CE_BIND, &bind, sizeof(bind));
	if (result == CE_RESULT_OK && !sol_evidence_run_record_bind_v1(sol.evidence,
		index, (uint32_t) entity, bind.client_generation))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH, "SOL successful bind route failed closed.\n");
		return;
	}
	if (result != CE_RESULT_OK || !bind.client_generation
		|| strcmp(bind.observed_player_name, identity->player_name)
		|| strcmp(bind.observed_team, SOL_TEAM))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH,
			"SOL evidence bind failed closed (%d, name=%s, team=%s).\n",
			result, bind.observed_player_name, bind.observed_team);
		return;
	}
	if (!sol_candidate_registry_bind_v1(sol.candidates, index,
		bind.client_generation, call_observation, NULL))
	{
		sol_evidence_run_fail_stop_v1(sol.evidence);
		G_sprint(self, PRINT_HIGH,
			"SOL observation profile query failed closed for slot %d generation %u.\n",
			entity, bind.client_generation);
		return;
	}
	profile = sol_candidate_registry_profile_v1(sol.candidates, index);
	G_sprint(self, PRINT_HIGH,
		"SOL plan seat %s (%s) bound to %s/%s generation %u with COV batch cap %u.\n",
		identity->plan_seat, identity->evidence_seat, identity->player_name, SOL_TEAM,
		bind.client_generation, profile ? profile->max_batch_bytes : 0u);
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

void Sol_RemoveAll(void)
{
	if (sol.evidence && sol_evidence_run_active_v1(sol.evidence))
	{
		sol_evidence_run_request_close_v1(sol.evidence);
		return;
	}
	reset_runtime_candidates();
}
