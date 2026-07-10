#include "sol_observation_client.h"

#include "sol_wire.h"

#include <stdlib.h>
#include <string.h>

#define SOL_OBSERVATION_MAX_CLIENTS_V1 32u
#define SOL_OBSERVATION_MAX_SEEN_V1 96u
#define SOL_OBSERVATION_MAX_ANCHORS_V1 16u
#define SOL_OBSERVATION_MAX_ASYNC_V1 128u

struct sol_observation_client_v1 {
	cov_profile_v1 profile;
	cov_get_committed_v1 get;
	sol_observation_call_v1 call;
	void *call_context;
	uint64_t next_frame_seq;
	size_t sealed_length;
	float last_self_view[3];
	int has_self_view;
	int failed;
};

static void init_header(cov_payload_header_v1 *header, size_t size)
{
	header->protocol_version = COV_PROTOCOL_VERSION_V1;
	header->struct_size = (uint32_t)size;
}

static int identity_nonzero(const uint8_t identity[32])
{
	uint8_t any = 0;
	size_t i;

	for (i = 0; i < 32u; ++i)
		any |= identity[i];
	return any != 0;
}

static int profile_valid(const cov_profile_v1 *profile, uint32_t engine_slot,
	uint32_t client_generation)
{
	return profile->header.protocol_version == COV_PROTOCOL_VERSION_V1 &&
		profile->header.struct_size == sizeof(*profile) &&
		profile->engine_slot == engine_slot &&
		profile->client_generation == client_generation &&
		identity_nonzero(profile->static_asset_set_id) &&
		identity_nonzero(profile->sensory_profile_id) &&
		profile->max_batch_bytes == COV_MAX_BATCH_BYTES_V1 &&
		profile->max_seen_entities == SOL_OBSERVATION_MAX_SEEN_V1 &&
		profile->max_static_anchors == SOL_OBSERVATION_MAX_ANCHORS_V1 &&
		profile->max_async_events == SOL_OBSERVATION_MAX_ASYNC_V1;
}

static uint32_t read_u32(const uint8_t *wire)
{
	return (uint32_t)wire[0] | ((uint32_t)wire[1] << 8) |
		((uint32_t)wire[2] << 16) | ((uint32_t)wire[3] << 24);
}

static uint16_t read_u16(const uint8_t *wire)
{
	return (uint16_t)wire[0] | ((uint16_t)wire[1] << 8);
}

static uint64_t read_u64(const uint8_t *wire)
{
	return (uint64_t)read_u32(wire) |
		((uint64_t)read_u32(wire + 4u) << 32);
}

static int get_route_unchanged(const sol_observation_client_v1 *client,
	uint64_t frame_seq, uint32_t dt_us)
{
	const cov_get_committed_v1 *get = &client->get;

	return get->header.protocol_version == COV_PROTOCOL_VERSION_V1 &&
		get->header.struct_size == sizeof(*get) &&
		get->engine_slot == client->profile.engine_slot &&
		get->client_generation == client->profile.client_generation &&
		get->frame_seq == frame_seq && get->dt_us == dt_us &&
		get->output_capacity == COV_MAX_BATCH_BYTES_V1;
}

static int batch_matches_request(const sol_observation_client_v1 *client,
	uint64_t frame_seq, uint32_t dt_us)
{
	const uint8_t *batch = client->get.batch;

	return read_u64(batch + 4u) == frame_seq &&
		read_u32(batch + 12u) == dt_us &&
		!memcmp(batch + 16u, client->profile.static_asset_set_id, 32u) &&
		!memcmp(batch + 48u, client->profile.sensory_profile_id, 32u);
}

static sol_observation_status_v1 quarantine(sol_observation_client_v1 *client)
{
	client->sealed_length = 0;
	client->failed = 1;
	return SOL_OBSERVATION_INVALID;
}

static float observation_angle(uint16_t encoded)
{
	float angle = (float)encoded * (360.0f / 65536.0f);

	return encoded >= UINT16_C(0x8000) ? angle - 360.0f : angle;
}

static void retain_self_view(sol_observation_client_v1 *client)
{
	unsigned i;

	for (i = 0; i < 3u; ++i)
		client->last_self_view[i] = observation_angle(
			read_u16(client->get.batch + 130u + (i * 2u)));
	if (client->last_self_view[0] < -90.0f)
		client->last_self_view[0] = -90.0f;
	else if (client->last_self_view[0] > 90.0f)
		client->last_self_view[0] = 90.0f;
	client->has_self_view = 1;
}

sol_observation_client_v1 *sol_observation_client_create_v1(
	uint32_t engine_slot, uint32_t client_generation,
	sol_observation_call_v1 call, void *call_context)
{
	sol_observation_client_v1 *client;
	intptr_t result;

	if (!call || engine_slot == 0u ||
		engine_slot > SOL_OBSERVATION_MAX_CLIENTS_V1 || client_generation == 0u)
		return NULL;
	client = calloc(1u, sizeof(*client));
	if (!client)
		return NULL;
	client->call = call;
	client->call_context = call_context;
	init_header(&client->profile.header, sizeof(client->profile));
	client->profile.engine_slot = engine_slot;
	client->profile.client_generation = client_generation;
	result = client->call(client->call_context, COV_GET_PROFILE,
		&client->profile, (intptr_t)sizeof(client->profile));
	if (result != COV_RESULT_OK ||
		!profile_valid(&client->profile, engine_slot, client_generation)) {
		sol_observation_client_destroy_v1(client);
		return NULL;
	}
	return client;
}

void sol_observation_client_destroy_v1(sol_observation_client_v1 *client)
{
	if (client)
		memset(client, 0, sizeof(*client));
	free(client);
}

const cov_profile_v1 *sol_observation_client_profile_v1(
	const sol_observation_client_v1 *client)
{
	return client ? &client->profile : NULL;
}

sol_observation_status_v1 sol_observation_client_poll_v1(
	sol_observation_client_v1 *client, uint32_t dt_us)
{
	uint64_t frame_seq;
	intptr_t result;

	if (!client)
		return SOL_OBSERVATION_INVALID;
	client->sealed_length = 0;
	if (client->failed)
		return SOL_OBSERVATION_INVALID;
	if (!dt_us || client->next_frame_seq == UINT64_MAX)
		return quarantine(client);
	frame_seq = client->next_frame_seq;
	init_header(&client->get.header, sizeof(client->get));
	client->get.engine_slot = client->profile.engine_slot;
	client->get.client_generation = client->profile.client_generation;
	client->get.frame_seq = frame_seq;
	client->get.dt_us = dt_us;
	client->get.output_capacity = COV_MAX_BATCH_BYTES_V1;
	client->get.output_length = 0;
	result = client->call(client->call_context, COV_GET_COMMITTED,
		&client->get, (intptr_t)sizeof(client->get));
	if (!get_route_unchanged(client, frame_seq, dt_us))
		return quarantine(client);
	if (result == COV_RESULT_EMPTY)
		return client->get.output_length == 0u ? SOL_OBSERVATION_EMPTY :
			quarantine(client);
	if (result != COV_RESULT_OK || client->get.output_length == 0u ||
		client->get.output_length > COV_MAX_BATCH_BYTES_V1 ||
		!sol_wire_observation_is_canonical_v1(client->get.batch,
			client->get.output_length) ||
		!batch_matches_request(client, frame_seq, dt_us))
		return quarantine(client);
	retain_self_view(client);
	client->sealed_length = client->get.output_length;
	client->next_frame_seq++;
	return SOL_OBSERVATION_READY;
}

const uint8_t *sol_observation_client_batch_v1(
	const sol_observation_client_v1 *client, size_t *length)
{
	if (length)
		*length = 0;
	if (!client || !client->sealed_length)
		return NULL;
	if (length)
		*length = client->sealed_length;
	return client->get.batch;
}

int sol_observation_client_neutral_view_v1(
	const sol_observation_client_v1 *client, float output[3])
{
	if (!output)
		return 0;
	memset(output, 0, sizeof(float) * 3u);
	if (!client || !client->has_self_view)
		return 0;
	memcpy(output, client->last_self_view, sizeof(client->last_self_view));
	return 1;
}

uint64_t sol_observation_client_next_frame_v1(
	const sol_observation_client_v1 *client)
{
	return client ? client->next_frame_seq : 0u;
}
