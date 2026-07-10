#include "sol_observation_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(cov_payload_header_v1) == 8u,
	"COV v1 native header ABI changed");
_Static_assert(sizeof(cov_profile_v1) == 96u,
	"COV v1 native profile ABI changed");
_Static_assert(offsetof(cov_get_committed_v1, batch) == 36u,
	"COV v1 native GET prefix ABI changed");

static const uint8_t asset_id[32] = {
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
	0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11
};
static const uint8_t sensory_id[32] = {
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22
};
static const char observation_golden_hex[] =
	"534f42310000000000000000c8320000"
	"1111111111111111111111111111111111111111111111111111111111111111"
	"2222222222222222222222222222222222222222222222222222222222222222"
	"01000000000000000001010000000000000002000000"
	"0000000000000000000100"
	"0101020100f8ff10001800640038ff0000000000400080b000"
	"640000003200000007000000190000001e000000050000000c000000"
	"002000402000000011000304000d04"
	"0100000000000000010101"
	"00000000000000000000";

typedef struct fake_host_v1 {
	uint32_t slot;
	uint32_t generation;
	uint32_t health;
	int profile_calls;
	int profile_mode;
	int poll_calls;
	int empty_polls;
	int poll_mode;
	uint64_t requested_frames[4];
	intptr_t last_operation;
	intptr_t last_payload_size;
} fake_host_v1;

static void require(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static size_t hex_to_bytes(const char *hex, uint8_t *output, size_t capacity)
{
	size_t length = strlen(hex) / 2u;
	size_t i;

	require(strlen(hex) % 2u == 0u && length <= capacity,
		"observation fixture has valid bounded length");
	for (i = 0; i < length; ++i) {
		unsigned value;
		require(sscanf(hex + (i * 2u), "%2x", &value) == 1,
			"observation fixture parses");
		output[i] = (uint8_t)value;
	}
	return length;
}

static void fixture_u32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
	output[2] = (uint8_t)(value >> 16);
	output[3] = (uint8_t)(value >> 24);
}

static void fixture_u64(uint8_t *output, uint64_t value)
{
	fixture_u32(output, (uint32_t)value);
	fixture_u32(output + 4u, (uint32_t)(value >> 32));
}

static intptr_t fake_profile_call(void *context, intptr_t operation,
	void *payload, intptr_t payload_size)
{
	fake_host_v1 *host = context;
	cov_profile_v1 *profile = payload;

	host->last_operation = operation;
	host->last_payload_size = payload_size;
	if (operation == COV_GET_PROFILE) {
		if (payload_size != (intptr_t)sizeof(*profile) ||
			profile->header.protocol_version != COV_PROTOCOL_VERSION_V1 ||
			profile->header.struct_size != sizeof(*profile) ||
			profile->engine_slot != host->slot ||
			profile->client_generation != host->generation)
			return COV_RESULT_INVALID;
		host->profile_calls++;
		memcpy(profile->static_asset_set_id, asset_id, sizeof(asset_id));
		memcpy(profile->sensory_profile_id, sensory_id, sizeof(sensory_id));
		profile->max_batch_bytes = COV_MAX_BATCH_BYTES_V1;
		profile->max_seen_entities = 96u;
		profile->max_static_anchors = 16u;
		profile->max_async_events = 128u;
		if (host->profile_mode == 1)
			profile->header.struct_size--;
		else if (host->profile_mode == 2)
			profile->max_batch_bytes--;
		else if (host->profile_mode == 3)
			profile->client_generation++;
		return COV_RESULT_OK;
	}
	if (operation == COV_GET_COMMITTED) {
		cov_get_committed_v1 *get = payload;
		size_t length;

		if (payload_size != (intptr_t)sizeof(*get) ||
			get->header.protocol_version != COV_PROTOCOL_VERSION_V1 ||
			get->header.struct_size != sizeof(*get) ||
			get->engine_slot != host->slot ||
			get->client_generation != host->generation || !get->dt_us ||
			get->output_capacity != COV_MAX_BATCH_BYTES_V1 ||
			host->poll_calls >= (int)(sizeof(host->requested_frames) /
				sizeof(host->requested_frames[0])))
			return COV_RESULT_INVALID;
		host->requested_frames[host->poll_calls++] = get->frame_seq;
		if (host->empty_polls > 0) {
			host->empty_polls--;
			get->output_length = 0;
			return COV_RESULT_EMPTY;
		}
		length = hex_to_bytes(observation_golden_hex, get->batch,
			get->output_capacity);
		fixture_u64(get->batch + 4u, get->frame_seq);
		fixture_u32(get->batch + 12u, get->dt_us);
		fixture_u32(get->batch + 138u, host->health);
		get->output_length = (uint32_t)length;
		if (host->poll_mode == 1)
			get->client_generation++;
		else if (host->poll_mode == 2) {
			get->batch[length] = 0;
			get->output_length++;
		}
		else if (host->poll_mode == 3)
			get->batch[16] ^= 1u;
		else if (host->poll_mode == 4) {
			get->output_length = 0;
			return COV_RESULT_INVALID;
		}
		return COV_RESULT_OK;
	}
	return COV_RESULT_INVALID;
}

static void test_profile_query_is_generation_bound_and_exact(void)
{
	fake_host_v1 host = {0};
	sol_observation_client_v1 *client;
	const cov_profile_v1 *profile;

	host.slot = 7u;
	host.generation = 42u;
	host.health = 100u;
	client = sol_observation_client_create_v1(host.slot, host.generation,
		fake_profile_call, &host);

	require(client != NULL, "valid bound generation creates an observation client");
	profile = sol_observation_client_profile_v1(client);
	require(host.profile_calls == 1 && host.last_operation == COV_GET_PROFILE &&
		host.last_payload_size == (intptr_t)sizeof(cov_profile_v1),
		"bind issues exactly one closed profile operation and native size");
	require(profile != NULL && profile->engine_slot == host.slot &&
		profile->client_generation == host.generation &&
		!memcmp(profile->static_asset_set_id, asset_id, sizeof(asset_id)) &&
		!memcmp(profile->sensory_profile_id, sensory_id, sizeof(sensory_id)) &&
		profile->max_batch_bytes == COV_MAX_BATCH_BYTES_V1 &&
		profile->max_seen_entities == 96u &&
		profile->max_static_anchors == 16u &&
		profile->max_async_events == 128u,
		"profile is engine-authored and retains its generation route");
	sol_observation_client_destroy_v1(client);
}

static void test_empty_does_not_advance_and_later_commit_seals(void)
{
	fake_host_v1 host = {0};
	sol_observation_client_v1 *client;
	const uint8_t *batch;
	size_t length = 99u;
	float view[3] = {99.0f, 99.0f, 99.0f};

	host.slot = 2u;
	host.generation = 9u;
	host.health = 75u;
	host.empty_polls = 1;
	client = sol_observation_client_create_v1(host.slot, host.generation,
		fake_profile_call, &host);
	require(client != NULL, "empty-path client binds to its profile");
	require(sol_observation_client_poll_v1(client, 13000u) ==
		SOL_OBSERVATION_EMPTY, "first missing preceding epoch is explicitly empty");
	batch = sol_observation_client_batch_v1(client, &length);
	require(batch == NULL && length == 0u &&
		sol_observation_client_next_frame_v1(client) == 0u,
		"empty result exposes no bytes and does not consume frame zero");
	require(!sol_observation_client_neutral_view_v1(client, view) &&
		view[0] == 0.0f && view[1] == 0.0f && view[2] == 0.0f,
		"EMPTY before any batch supplies the fixed zero neutral view");
	require(sol_observation_client_poll_v1(client, 14000u) ==
		SOL_OBSERVATION_READY, "later complete epoch becomes ready");
	batch = sol_observation_client_batch_v1(client, &length);
	require(batch != NULL && length == 202u && !memcmp(batch, "SOB1", 4u) &&
		sol_observation_client_next_frame_v1(client) == 1u,
		"ready result seals one canonical batch and advances once");
	require(sol_observation_client_neutral_view_v1(client, view) &&
		view[0] == 0.0f && view[1] == 90.0f && view[2] == -180.0f,
		"later neutral fallback uses the last complete self view");
	require(host.poll_calls == 2 && host.requested_frames[0] == 0u &&
		host.requested_frames[1] == 0u,
		"retry after EMPTY remains generation-bound to frame zero");
	sol_observation_client_destroy_v1(client);
}

static void test_two_slots_poll_in_reverse_without_aliasing(void)
{
	fake_host_v1 first_host = {0};
	fake_host_v1 second_host = {0};
	sol_observation_client_v1 *first;
	sol_observation_client_v1 *second;
	const uint8_t *first_batch;
	const uint8_t *second_batch;
	size_t first_length = 0;
	size_t second_length = 0;

	first_host.slot = 1u;
	first_host.generation = 3u;
	first_host.health = 100u;
	second_host.slot = 2u;
	second_host.generation = 4u;
	second_host.health = 25u;
	first = sol_observation_client_create_v1(first_host.slot,
		first_host.generation, fake_profile_call, &first_host);
	second = sol_observation_client_create_v1(second_host.slot,
		second_host.generation, fake_profile_call, &second_host);
	require(first != NULL && second != NULL,
		"two generations own independent observation clients");
	require(sol_observation_client_poll_v1(second, 13000u) ==
		SOL_OBSERVATION_READY &&
		sol_observation_client_poll_v1(first, 13000u) == SOL_OBSERVATION_READY,
		"reverse polling seals both slots independently");
	first_batch = sol_observation_client_batch_v1(first, &first_length);
	second_batch = sol_observation_client_batch_v1(second, &second_length);
	require(first_batch != NULL && second_batch != NULL &&
		first_batch != second_batch && first_length == 202u &&
		second_length == 202u && memcmp(first_batch, second_batch, 202u) != 0,
		"per-client 65,535-byte payload storage cannot alias another slot");
	require(first_batch[138] == 100u && second_batch[138] == 25u,
		"named self difference remains private to its recipient batch");
	sol_observation_client_destroy_v1(first);
	sol_observation_client_destroy_v1(second);
}

static void test_stale_route_quarantines_without_recovery(void)
{
	fake_host_v1 host = {0};
	sol_observation_client_v1 *client;
	size_t length = 99u;

	host.slot = 3u;
	host.generation = 8u;
	host.health = 50u;
	host.poll_mode = 1;
	client = sol_observation_client_create_v1(host.slot, host.generation,
		fake_profile_call, &host);
	require(client != NULL, "stale-route client first binds normally");
	require(sol_observation_client_poll_v1(client, 13000u) ==
		SOL_OBSERVATION_INVALID &&
		sol_observation_client_batch_v1(client, &length) == NULL && length == 0u,
		"mutated generation route fails closed without exposing bytes");
	host.poll_mode = 0;
	require(sol_observation_client_poll_v1(client, 13000u) ==
		SOL_OBSERVATION_INVALID && host.poll_calls == 1,
		"stale route permanently quarantines this generation");
	sol_observation_client_destroy_v1(client);
}

static void test_malformed_profile_and_batch_fail_closed(void)
{
	fake_host_v1 host = {0};
	sol_observation_client_v1 *client;
	size_t length = 99u;
	int mode;

	host.slot = 4u;
	host.generation = 12u;
	host.health = 40u;
	for (mode = 1; mode <= 3; ++mode) {
		host.profile_mode = mode;
		client = sol_observation_client_create_v1(host.slot, host.generation,
			fake_profile_call, &host);
		require(client == NULL,
			"mutated native profile header, capacity, or generation rejects");
	}
	host.profile_mode = 0;
	host.poll_mode = 2;
	client = sol_observation_client_create_v1(host.slot, host.generation,
		fake_profile_call, &host);
	require(client != NULL, "malformed-batch client starts from a valid profile");
	require(sol_observation_client_poll_v1(client, 13000u) ==
		SOL_OBSERVATION_INVALID &&
		sol_observation_client_batch_v1(client, &length) == NULL && length == 0u,
		"trailing observation byte invalidates the whole sealed batch");
	sol_observation_client_destroy_v1(client);
}

int main(void)
{
	test_profile_query_is_generation_bound_and_exact();
	test_empty_does_not_advance_and_later_commit_seals();
	test_two_slots_poll_in_reverse_without_aliasing();
	test_stale_route_quarantines_without_recovery();
	test_malformed_profile_and_batch_fail_closed();
	printf("sol_observation_client: 5 contract tests passed\n");
	return 0;
}
