#include "sol_wire.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
		"hex fixture has valid bounded length");
	for (i = 0; i < length; ++i) {
		unsigned value;
		require(sscanf(hex + (i * 2u), "%2x", &value) == 1,
			"hex fixture parses");
		output[i] = (uint8_t)value;
	}
	return length;
}

static void fixture_u16(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
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

static size_t append_killfeed_fixture(uint8_t *output, size_t capacity,
	size_t offset, uint64_t sequence, uint16_t text_length)
{
	size_t required = 13u + text_length;

	require(offset <= capacity && required <= capacity - offset,
		"large observation fixture stays in bounds");
	fixture_u64(output + offset, sequence);
	offset += 8u;
	output[offset++] = 4u;
	fixture_u16(output + offset, UINT16_C(0x0401));
	offset += 2u;
	fixture_u16(output + offset, text_length);
	offset += 2u;
	memset(output + offset, 'k', text_length);
	return offset + text_length;
}

static size_t build_large_observation_fixture(uint8_t *output, size_t capacity,
	uint16_t short_text_length)
{
	size_t offset;
	uint64_t sequence = 2u;
	unsigned i;

	require(short_text_length == 52u || short_text_length == 53u,
		"large observation uses a worked boundary payload");
	offset = hex_to_bytes(observation_golden_hex, output, capacity);
	require(offset == 202u, "large observation starts from the golden batch");
	fixture_u64(output + 90u, 65u);
	fixture_u32(output + 98u, 66u);
	offset = append_killfeed_fixture(output, capacity, offset, sequence++,
		short_text_length);
	for (i = 0; i < 63u; ++i) {
		offset = append_killfeed_fixture(output, capacity, offset, sequence++,
			1023u);
	}
	require(sequence == 66u, "large observation has contiguous event sequences");
	return offset;
}

static sol_action_response_v1 known_action(void)
{
	sol_action_response_v1 action = {0};

	action.frame_seq = 7;
	action.view_angles[0] = 1.0f;
	action.view_angles[1] = -2.5f;
	action.forwardmove = 400;
	action.sidemove = -500;
	action.upmove = 6;
	action.buttons = 9;
	action.weapon_select = SOL_WEAPON_RL;
	return action;
}

static void test_independent_observation_fixture(void)
{
	uint8_t bytes[203];
	size_t length = hex_to_bytes(observation_golden_hex, bytes, sizeof(bytes));
	uint8_t saved;

	require(length == 202, "independent mvdsv fixture is 202 bytes");
	require(sol_wire_observation_is_canonical_v1(bytes, length),
		"independent mvdsv-produced fixture validates in KTX reader");
	bytes[length] = 0;
	require(!sol_wire_observation_is_canonical_v1(bytes, length + 1u),
		"KTX reader rejects a trailing byte");
	saved = bytes[110];
	bytes[110] = 4;
	require(!sol_wire_observation_is_canonical_v1(bytes, length),
		"KTX reader rejects a channel/kind mismatch");
	bytes[110] = saved;
	require(sol_wire_observation_is_canonical_v1(bytes, length),
		"restored observation remains canonical");
}

static void test_observation_batch_size_boundary(void)
{
	uint8_t *wire = malloc(SOL_WIRE_MAX_BATCH_V1 + 1u);
	size_t length;

	require(wire != NULL, "large observation fixture allocates");
	length = build_large_observation_fixture(wire,
		SOL_WIRE_MAX_BATCH_V1 + 1u, 52u);
	require(length == SOL_WIRE_MAX_BATCH_V1,
		"worked canonical observation is exactly 65,535 bytes");
	require(sol_wire_observation_is_canonical_v1(wire, length),
		"reader accepts a canonical observation at the maximum batch size");
	length = build_large_observation_fixture(wire,
		SOL_WIRE_MAX_BATCH_V1 + 1u, 53u);
	require(length == SOL_WIRE_MAX_BATCH_V1 + 1u,
		"worked oversized observation is exactly 65,536 bytes");
	require(!sol_wire_observation_is_canonical_v1(wire, length),
		"reader rejects an otherwise canonical observation above the maximum");
	free(wire);
}

static void test_action_golden_and_round_trip(void)
{
	static const uint8_t expected[SOL_WIRE_ACTION_BASE_V1] = {
		'S', 'A', 'C', '1', 7, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0x80, 0x3f, 0, 0, 0x20, 0xc0, 0, 0, 0, 0,
		0x90, 0x01, 0x0c, 0xfe, 0x06, 0, 9, 7, 0
	};
	sol_action_response_v1 action = known_action();
	sol_action_response_v1 decoded;
	uint8_t wire[SOL_WIRE_ACTION_MAX_V1];
	size_t length = 999;

	require(sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"complete action encodes");
	require(length == sizeof(expected) && !memcmp(wire, expected, sizeof(expected)),
		"action matches independent SAC1 bytes");
	memset(&decoded, 0xa5, sizeof(decoded));
	require(sol_wire_decode_action_v1(wire, length, &decoded),
		"canonical action decodes");
	require(decoded.frame_seq == action.frame_seq &&
		decoded.forwardmove == action.forwardmove &&
		decoded.sidemove == action.sidemove && decoded.upmove == action.upmove &&
		decoded.buttons == action.buttons &&
		decoded.weapon_select == action.weapon_select && !decoded.teamsay_present,
		"round trip preserves complete control");
}

static void test_negative_zero_normalizes_and_noncanonical_wire_rejects(void)
{
	sol_action_response_v1 action = known_action();
	sol_action_response_v1 decoded;
	uint8_t wire[SOL_WIRE_ACTION_MAX_V1];
	size_t length = 0;

	action.view_angles[2] = -0.0f;
	require(sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"writer normalizes negative zero");
	require(wire[20] == 0 && wire[21] == 0 && wire[22] == 0 && wire[23] == 0,
		"negative zero emits canonical positive-zero bits");
	wire[23] = 0x80;
	memset(&decoded, 0xa5, sizeof(decoded));
	require(!sol_wire_decode_action_v1(wire, length, &decoded),
		"reader rejects negative-zero wire");
	require(decoded.frame_seq == 0 && decoded.buttons == 0 &&
		decoded.teamsay_length == 0,
		"failed decode leaves a zeroed response");
	action = known_action();
	action.view_angles[0] = NAN;
	require(!sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length) &&
		length == 0, "writer rejects non-finite control without partial output");
}

static void test_closed_control_and_chat_validation(void)
{
	static const uint8_t valid_message[] = "quad seen low";
	static const uint8_t reserved_message[] = "s-p hidden diversion";
	static const uint8_t unsafe_message[] = "quad;$weapon";
	sol_action_response_v1 action = known_action();
	sol_action_response_v1 decoded;
	uint8_t wire[SOL_WIRE_ACTION_MAX_V1 + 1u];
	size_t length = 0;

	action.buttons = 0x10;
	require(!sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"unknown button bits reject the complete control");
	action = known_action();
	action.weapon_select = 9;
	require(!sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"raw impulse outside closed weapon enum is rejected");
	action = known_action();
	action.teamsay_present = 1;
	action.teamsay_length = sizeof(valid_message) - 1u;
	memcpy(action.teamsay, valid_message, action.teamsay_length);
	require(sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"safe bounded teamsay encodes with valid movement");
	require(length == SOL_WIRE_ACTION_BASE_V1 + 2u + action.teamsay_length,
		"chat action has exact option/text length");
	require(sol_wire_decode_action_v1(wire, length, &decoded) &&
		decoded.teamsay_present && decoded.teamsay_length == action.teamsay_length &&
		!memcmp(decoded.teamsay, valid_message, action.teamsay_length),
		"safe teamsay round trips exactly");
	wire[length] = 0;
	require(!sol_wire_decode_action_v1(wire, length + 1u, &decoded),
		"action reader rejects trailing bytes");
	action.teamsay_length = sizeof(reserved_message) - 1u;
	memcpy(action.teamsay, reserved_message, action.teamsay_length);
	require(!sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"reserved KTX diversion first token is rejected");
	action.teamsay_length = sizeof(unsafe_message) - 1u;
	memcpy(action.teamsay, unsafe_message, action.teamsay_length);
	require(!sol_wire_encode_action_v1(&action, wire, sizeof(wire), &length),
		"semicolon and dollar expansion bytes are rejected");
}

int main(void)
{
	test_independent_observation_fixture();
	test_observation_batch_size_boundary();
	test_action_golden_and_round_trip();
	test_negative_zero_normalizes_and_noncanonical_wire_rejects();
	test_closed_control_and_chat_validation();
	printf("sol_wire: 5 contract tests passed\n");
	return 0;
}
