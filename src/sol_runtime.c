#include "g_local.h"

#include "controller_evidence_protocol.h"
#include "controller_observation_protocol.h"
#include "sol_ktx_adapter.h"
#include "sol_observation_client.h"
#include "sol_runtime.h"

#include <string.h>

#define SOL_PLAYER_NAME "cand-1"
#define SOL_TEAM "red"

typedef enum sol_stage
{
	SOL_STAGE_EMPTY = 0,
	SOL_STAGE_PENDING,
	SOL_STAGE_EXPECTING_CLIENT,
	SOL_STAGE_CLAIMED,
	SOL_STAGE_BOUND
} sol_stage;

typedef struct sol_registry
{
	sol_stage stage;
	int entity;
	uint32_t client_generation;
	int epoch_open;
	int bound;
	int closed;
	char run_nonce[CE_RUN_NONCE_CAP];
	char epoch_kind[CE_EPOCH_KIND_CAP];
	char plan_seat[CE_SEAT_ID_CAP];
	char seat_nonce[CE_SEAT_NONCE_CAP];
	char controller_id[CE_CONTROLLER_ID_CAP];
	char controller_version[CE_CONTROLLER_VERSION_CAP];
	char controller_digest[CE_SHA256_ID_CAP];
	char build_id[CE_BUILD_ID_CAP];
	char config_sha256[CE_SHA256_HEX_CAP];
	char treatment_digest[CE_SHA256_ID_CAP];
	char writer_id[CE_WRITER_ID_CAP];
	sol_observation_client_v1 *observation;
	uint64_t diagnostic_frame_seq;
} sol_registry;

static sol_registry sol;

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

static intptr_t call_observation(void *context, intptr_t operation,
		void *payload, intptr_t payload_size)
{
	(void) context;
	return trap_ControllerObservationV1(operation, payload, payload_size);
}

static void reset_registry(void)
{
	sol_observation_client_destroy_v1(sol.observation);
	memset(&sol, 0, sizeof(sol));
}

static int close_evidence(void)
{
	ce_epoch_end_v1 end;
	ce_unbind_v1 unbind;
	int end_result = CE_RESULT_IGNORED;
	int unbind_result = CE_RESULT_IGNORED;

	if (sol.closed)
	{
		return CE_RESULT_OK;
	}
	if (sol.epoch_open)
	{
		memset(&end, 0, sizeof(end));
		init_header(&end.header, sizeof(end));
		strlcpy(end.run_nonce, sol.run_nonce, sizeof(end.run_nonce));
		end_result = trap_ControllerEvidenceV1(CE_MATCH_END, &end, sizeof(end));
		sol.epoch_open = 0;
	}
	if (sol.bound)
	{
		memset(&unbind, 0, sizeof(unbind));
		init_header(&unbind.header, sizeof(unbind));
		unbind.engine_slot = (uint32_t) sol.entity;
		unbind.client_generation = sol.client_generation;
		strlcpy(unbind.run_nonce, sol.run_nonce, sizeof(unbind.run_nonce));
		strlcpy(unbind.seat_nonce, sol.seat_nonce, sizeof(unbind.seat_nonce));
		unbind_result = trap_ControllerEvidenceV1(CE_UNBIND, &unbind, sizeof(unbind));
		sol.bound = 0;
	}
	sol_observation_client_destroy_v1(sol.observation);
	sol.observation = NULL;
	sol.closed = 1;
	G_cprint("[sol-slice/v1] close end=%d unbind=%d slot=%d\n",
			end_result, unbind_result, sol.entity);
	return end_result == CE_RESULT_OK
			&& (!sol.client_generation || unbind_result == CE_RESULT_OK) ? CE_RESULT_OK
			: CE_RESULT_INVALID;
}

int Sol_IsClient(const struct gedict_s *client)
{
	return client && sol.entity >= 1 && sol.entity <= MAX_CLIENTS
			&& client == &g_edicts[sol.entity]
			&& sol.stage >= SOL_STAGE_CLAIMED;
}

static int claim_client(gedict_t *client)
{
	int entity;

	if (!client || !client->isBot || sol.stage != SOL_STAGE_EXPECTING_CLIENT
			|| !client->netname || strcmp(client->netname, SOL_PLAYER_NAME))
	{
		return 0;
	}
	entity = NUM_FOR_EDICT(client);
	if (entity < 1 || entity > MAX_CLIENTS)
	{
		return 0;
	}
	sol.entity = entity;
	sol.stage = SOL_STAGE_CLAIMED;
	client->blocked = 0;
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
	if (!Sol_IsClient(client))
	{
		return;
	}
	if (!sol.closed && evidence_extension_available())
	{
		close_evidence();
	}
	G_cprint("[sol-slice/v1] disconnect slot=%d\n", sol.entity);
	reset_registry();
}

static uint8_t command_msec(void)
{
	int msec = (int) (g_globalvars.frametime * 1000.0f + 0.5f);

	return (uint8_t) bound(1, msec, 255);
}

static void neutral_command(uint8_t msec,
		const sol_observation_client_v1 *observation,
		sol_ktx_command_v1 *command)
{
	memset(command, 0, sizeof(*command));
	command->msec = msec;
	sol_observation_client_neutral_view_v1(observation, command->angles);
}

void Sol_StartFrame(void)
{
	uint8_t command_wire[SOL_KTX_COMMAND_V1_SIZE];
	ce_frame_request_v1 request;
	sol_ktx_command_v1 command;
	const uint8_t *batch;
	gedict_t *client;
	sol_observation_status_v1 observation_status;
	size_t batch_length = 0;
	uint8_t msec;
	uint32_t dt_us;
	int evidence_result;

	if (sol.stage < SOL_STAGE_BOUND || !sol.bound || !sol.observation ||
			sol.entity < 1 || sol.entity > MAX_CLIENTS)
	{
		return;
	}
	client = &g_edicts[sol.entity];
	if (!client->isBot)
	{
		return;
	}
	msec = command_msec();
	dt_us = (uint32_t) msec * 1000u;

	/* Poll/seal phase. The current runtime registry admits one bound SOL slot. */
	observation_status = sol_observation_client_poll_v1(sol.observation, dt_us);
	batch = sol_observation_client_batch_v1(sol.observation, &batch_length);
	if (observation_status != SOL_OBSERVATION_READY)
	{
		batch = NULL;
		batch_length = 0;
	}

	/* Policy phase. No legacy tracer core may consume a COV batch. */
	neutral_command(msec, sol.observation, &command);
	if (!sol_ktx_encode_command_v1(&command, command_wire))
	{
		neutral_command(msec, NULL, &command);
		sol_ktx_encode_command_v1(&command, command_wire);
	}

	/* Command phase. Chat remains suppressed while no SOB1/SAC1 core is attached. */
	memset(&request, 0, sizeof(request));
	init_header(&request.header, sizeof(request));
	request.engine_slot = (uint32_t) sol.entity;
	request.client_generation = sol.client_generation;
	memcpy(request.requested_command.bytes, command_wire, sizeof(command_wire));
	evidence_result = trap_ControllerEvidenceV1(CE_FRAME_REQUEST, &request, sizeof(request));
	trap_SetBotCMD(sol.entity, command.msec, command.angles[0], command.angles[1],
			command.angles[2], command.forwardmove, command.sidemove, command.upmove,
			command.buttons, command.impulse);

	G_cprint("[sol-slice/v1] frame=%llu observation=%d bytes=%lu "
			"policy=neutral evidence=%d yaw=%.1f move=%d,%d,%d\n",
			(unsigned long long) sol.diagnostic_frame_seq++, (int) observation_status,
			(unsigned long) (batch ? batch_length : 0u), evidence_result,
			command.angles[1], command.forwardmove, command.sidemove, command.upmove);
}

int Sol_CommandBypassesBotGates(const char *command)
{
	return command && (!strcmp(command, "evidencebind") || !strcmp(command, "addsol")
			|| !strcmp(command, "evidenceclose") || !strcmp(command, "removeall"));
}

void Sol_EvidenceBind_f(void)
{
	sol_registry pending;
	ce_epoch_begin_v1 begin;
	char evidence_seat[CE_SEAT_ID_CAP];
	int result;

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
	if (sol.stage != SOL_STAGE_EMPTY)
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence seat already pending or active.\n");
		return;
	}
	memset(&pending, 0, sizeof(pending));
	if (!read_arg(2, pending.plan_seat, sizeof(pending.plan_seat))
			|| !sol_ktx_plan_seat_v1(pending.plan_seat, evidence_seat,
					sizeof(evidence_seat))
			|| !read_sha256_hex_arg(3, pending.run_nonce)
			|| !read_arg(4, pending.epoch_kind, sizeof(pending.epoch_kind))
			|| (strcmp(pending.epoch_kind, "diagnostic-client-lifecycle/v1")
					&& strcmp(pending.epoch_kind, "ktx-match/v1"))
			|| !read_sha256_hex_arg(5, pending.seat_nonce)
			|| !read_arg(6, pending.controller_id, sizeof(pending.controller_id))
			|| !read_arg(7, pending.controller_version, sizeof(pending.controller_version))
			|| !read_sha256_id_arg(8, pending.controller_digest,
					sizeof(pending.controller_digest))
			|| !read_sha256_id_arg(9, pending.build_id, sizeof(pending.build_id))
			|| !read_sha256_hex_arg(10, pending.config_sha256)
			|| !read_sha256_id_arg(11, pending.treatment_digest,
					sizeof(pending.treatment_digest))
			|| !read_arg(12, pending.writer_id, sizeof(pending.writer_id)))
	{
		G_sprint(self, PRINT_HIGH, "Invalid sealed SOL evidence arguments.\n");
		return;
	}

	memset(&begin, 0, sizeof(begin));
	init_header(&begin.header, sizeof(begin));
	strlcpy(begin.run_nonce, pending.run_nonce, sizeof(begin.run_nonce));
	strlcpy(begin.epoch_kind, pending.epoch_kind, sizeof(begin.epoch_kind));
	begin.epoch_id = 1;
	result = trap_ControllerEvidenceV1(CE_MATCH_BEGIN, &begin, sizeof(begin));
	if (result != CE_RESULT_OK)
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence epoch begin rejected (%d).\n", result);
		return;
	}
	pending.stage = SOL_STAGE_PENDING;
	pending.epoch_open = 1;
	sol = pending;
	G_sprint(self, PRINT_HIGH, "SOL plan seat %s pending.\n", sol.plan_seat);
}

void Sol_Add_f(void)
{
	char seat[32];
	char team[32];
	ce_bind_v1 bind;
	const cov_profile_v1 *profile;
	int entity;
	int result;

	if (trap_CmdArgc() != 4 || !read_arg(2, seat, sizeof(seat))
			|| !read_arg(3, team, sizeof(team)) || !sol_ktx_add_shape_v1(seat, team))
	{
		G_sprint(self, PRINT_HIGH, "Usage: /botcmd addsol 20 red\n");
		return;
	}
	if (sol.stage != SOL_STAGE_PENDING || !sol.epoch_open)
	{
		G_sprint(self, PRINT_HIGH, "Bind SOL evidence before adding seat 20.\n");
		return;
	}
	sol.stage = SOL_STAGE_EXPECTING_CLIENT;
	entity = (int) trap_AddBot(SOL_PLAYER_NAME, 4, 4, "base");
	if (!entity || sol.stage != SOL_STAGE_CLAIMED || sol.entity != entity)
	{
		G_sprint(self, PRINT_HIGH, "SOL client lifecycle claim failed.\n");
		if (entity)
		{
			trap_RemoveBot(entity);
		}
		else
		{
			sol.stage = SOL_STAGE_PENDING;
		}
		return;
	}
	g_edicts[entity].blocked = 0;
	trap_SetBotUserInfo(entity, "team", SOL_TEAM, 0);

	memset(&bind, 0, sizeof(bind));
	init_header(&bind.header, sizeof(bind));
	bind.engine_slot = (uint32_t) entity;
	strlcpy(bind.run_nonce, sol.run_nonce, sizeof(bind.run_nonce));
	if (!sol_ktx_plan_seat_v1(sol.plan_seat, bind.seat_id, sizeof(bind.seat_id)))
	{
		G_sprint(self, PRINT_HIGH, "SOL plan seat mapping failed.\n");
		trap_RemoveBot(entity);
		return;
	}
	strlcpy(bind.seat_nonce, sol.seat_nonce, sizeof(bind.seat_nonce));
	strlcpy(bind.controller_id, sol.controller_id, sizeof(bind.controller_id));
	strlcpy(bind.controller_version, sol.controller_version, sizeof(bind.controller_version));
	strlcpy(bind.controller_digest, sol.controller_digest, sizeof(bind.controller_digest));
	strlcpy(bind.build_id, sol.build_id, sizeof(bind.build_id));
	strlcpy(bind.config_sha256, sol.config_sha256, sizeof(bind.config_sha256));
	strlcpy(bind.treatment_digest, sol.treatment_digest, sizeof(bind.treatment_digest));
	strlcpy(bind.writer_id, sol.writer_id, sizeof(bind.writer_id));
	result = trap_ControllerEvidenceV1(CE_BIND, &bind, sizeof(bind));
	if (result != CE_RESULT_OK || bind.client_generation == 0
			|| strcmp(bind.observed_player_name, SOL_PLAYER_NAME)
			|| strcmp(bind.observed_team, SOL_TEAM))
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence bind rejected or engine identity mismatched "
				"(%d, name=%s, team=%s).\n", result, bind.observed_player_name,
				bind.observed_team);
		trap_RemoveBot(entity);
		return;
	}
	sol.client_generation = bind.client_generation;
	sol.bound = 1;
	sol.observation = sol_observation_client_create_v1((uint32_t) entity,
			bind.client_generation, call_observation, NULL);
	if (!sol.observation)
	{
		G_sprint(self, PRINT_HIGH,
				"SOL observation profile query rejected for slot %d generation %u.\n",
				entity, bind.client_generation);
		close_evidence();
		trap_RemoveBot(entity);
		if (sol.stage != SOL_STAGE_EMPTY)
		{
			reset_registry();
		}
		return;
	}
	profile = sol_observation_client_profile_v1(sol.observation);
	sol.stage = SOL_STAGE_BOUND;
	G_sprint(self, PRINT_HIGH, "SOL plan seat %s (%s) bound to %s/%s generation %u "
			"with COV batch cap %u.\n",
			sol.plan_seat, bind.seat_id, bind.observed_player_name, bind.observed_team,
			bind.client_generation, profile->max_batch_bytes);
}

void Sol_EvidenceClose_f(void)
{
	char run_nonce[CE_RUN_NONCE_CAP];

	if (trap_CmdArgc() != 3 || !read_arg(2, run_nonce, sizeof(run_nonce))
			|| !lowercase_hex64(run_nonce))
	{
		G_sprint(self, PRINT_HIGH, "Usage: /botcmd evidenceclose <run_nonce>\n");
		return;
	}
	if (sol.stage == SOL_STAGE_EMPTY || strcmp(run_nonce, sol.run_nonce))
	{
		G_sprint(self, PRINT_HIGH, "SOL run nonce does not match the active seat.\n");
		return;
	}
	if (sol.closed)
	{
		G_sprint(self, PRINT_HIGH, "SOL evidence already closed.\n");
		return;
	}
	G_sprint(self, PRINT_HIGH, "SOL evidence close result %d.\n", close_evidence());
}

void Sol_RemoveAll(void)
{
	int entity = sol.entity;

	if (sol.stage == SOL_STAGE_EMPTY)
	{
		return;
	}
	if (!sol.closed && evidence_extension_available())
	{
		close_evidence();
	}
	if (entity >= 1 && entity <= MAX_CLIENTS && g_edicts[entity].isBot)
	{
		trap_RemoveBot(entity);
	}
	if (sol.stage != SOL_STAGE_EMPTY)
	{
		reset_registry();
	}
}
