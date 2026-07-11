#include "sol_decision_trace.h"

#include "sol_wire.h"

#include <string.h>

_Static_assert(SOL_SHA256_DIGEST_SIZE_V1 == 32,
	"SDT1 v1 fixes each artifact digest at 32 bytes");
_Static_assert(SOL_DECISION_TRACE_BASE_SIZE_V1 == 4 + 8 + 32 + 32 + 3,
	"SDT1 v1 no-target wire size changed");
_Static_assert(SOL_DECISION_TRACE_TARGET_SIZE_V1 ==
	SOL_DECISION_TRACE_BASE_SIZE_V1 + 2,
	"SDT1 v1 target wire size changed");

static uint64_t read_u64_le(const uint8_t *input)
{
	uint64_t value = 0u;
	unsigned i;

	for (i = 0; i < 8u; ++i)
		value |= (uint64_t)input[i] << (i * 8u);
	return value;
}

static void write_u64_le(uint8_t *output, uint64_t value)
{
	unsigned i;

	for (i = 0; i < 8u; ++i)
		output[i] = (uint8_t)(value >> (i * 8u));
}

static uint16_t read_u16_le(const uint8_t *input)
{
	return (uint16_t)input[0] | ((uint16_t)input[1] << 8);
}

static void write_u16_le(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

int sol_decision_trace_decision_is_canonical_v1(
	const sol_decision_trace_decision_v1 *decision)
{
	if (!decision)
		return 0;
	switch (decision->decision_class) {
	case SOL_DECISION_CLASS_NEUTRAL_V1:
	case SOL_DECISION_CLASS_EXPLORE_V1:
		return decision->attack_gate == SOL_ATTACK_GATE_NONE_V1 &&
			decision->selected_sighting_tag ==
				SOL_SELECTED_SIGHTING_NONE_V1 &&
			decision->selected_sighting_token == 0u;
	case SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1:
		return decision->attack_gate ==
				SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1 &&
			decision->selected_sighting_tag ==
				SOL_SELECTED_SIGHTING_PRESENT_V1;
	case SOL_DECISION_CLASS_RESPAWN_V1:
		return decision->attack_gate == SOL_ATTACK_GATE_RESPAWN_ONLY_V1 &&
			decision->selected_sighting_tag ==
				SOL_SELECTED_SIGHTING_NONE_V1 &&
			decision->selected_sighting_token == 0u;
	default:
		return 0;
	}
}

int sol_decision_trace_encode_v1(const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length,
	const sol_decision_trace_decision_v1 *decision,
	uint8_t *output, size_t capacity, size_t *output_length)
{
	sol_action_response_v1 action;
	uint8_t encoded[SOL_DECISION_TRACE_TARGET_SIZE_V1];
	uint8_t sob1_sha256[SOL_SHA256_DIGEST_SIZE_V1];
	uint8_t sac1_sha256[SOL_SHA256_DIGEST_SIZE_V1];
	uint64_t observation_frame_seq;
	size_t required;

	if (output_length)
		*output_length = 0u;
	if (!output_length || !sob1 || !sac1 || !decision || !output ||
		!sol_decision_trace_decision_is_canonical_v1(decision) ||
		!sol_wire_observation_is_canonical_v1(sob1, sob1_length) ||
		!sol_wire_decode_action_v1(sac1, sac1_length, &action))
		return 0;
	observation_frame_seq = read_u64_le(sob1 + 4u);
	if (observation_frame_seq != action.frame_seq)
		return 0;
	required = decision->selected_sighting_tag ==
		SOL_SELECTED_SIGHTING_PRESENT_V1 ?
		SOL_DECISION_TRACE_TARGET_SIZE_V1 : SOL_DECISION_TRACE_BASE_SIZE_V1;
	if (capacity < required ||
		!sol_sha256_digest_v1(sob1, sob1_length, sob1_sha256) ||
		!sol_sha256_digest_v1(sac1, sac1_length, sac1_sha256))
		return 0;
	memcpy(encoded, "SDT1", 4u);
	write_u64_le(encoded + 4u, observation_frame_seq);
	memcpy(encoded + 12u, sob1_sha256, sizeof(sob1_sha256));
	memcpy(encoded + 44u, sac1_sha256, sizeof(sac1_sha256));
	encoded[76] = decision->decision_class;
	encoded[77] = decision->attack_gate;
	encoded[78] = decision->selected_sighting_tag;
	if (required == SOL_DECISION_TRACE_TARGET_SIZE_V1)
		write_u16_le(encoded + 79u, decision->selected_sighting_token);
	memcpy(output, encoded, required);
	*output_length = required;
	return 1;
}

int sol_decision_trace_decode_v1(const uint8_t *wire, size_t length,
	sol_decision_trace_v1 *output)
{
	sol_decision_trace_v1 decoded;
	size_t required;

	memset(&decoded, 0, sizeof(decoded));
	if (output)
		memset(output, 0, sizeof(*output));
	if (!wire || !output || length < SOL_DECISION_TRACE_BASE_SIZE_V1 ||
		memcmp(wire, "SDT1", 4u))
		return 0;
	decoded.observation_frame_seq = read_u64_le(wire + 4u);
	memcpy(decoded.sob1_sha256, wire + 12u, sizeof(decoded.sob1_sha256));
	memcpy(decoded.sac1_sha256, wire + 44u, sizeof(decoded.sac1_sha256));
	decoded.decision.decision_class = wire[76];
	decoded.decision.attack_gate = wire[77];
	decoded.decision.selected_sighting_tag = wire[78];
	if (decoded.decision.selected_sighting_tag ==
		SOL_SELECTED_SIGHTING_PRESENT_V1) {
		required = SOL_DECISION_TRACE_TARGET_SIZE_V1;
		if (length == required)
			decoded.decision.selected_sighting_token = read_u16_le(wire + 79u);
	}
	else if (decoded.decision.selected_sighting_tag ==
		SOL_SELECTED_SIGHTING_NONE_V1) {
		required = SOL_DECISION_TRACE_BASE_SIZE_V1;
	}
	else {
		return 0;
	}
	if (length != required ||
		!sol_decision_trace_decision_is_canonical_v1(&decoded.decision))
		return 0;
	*output = decoded;
	return 1;
}

int sol_decision_trace_verify_v1(const uint8_t *wire, size_t length,
	const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length)
{
	sol_decision_trace_v1 decoded;
	uint8_t expected[SOL_DECISION_TRACE_TARGET_SIZE_V1];
	size_t expected_length = 0u;

	if (!sol_decision_trace_decode_v1(wire, length, &decoded) ||
		!sol_decision_trace_encode_v1(sob1, sob1_length, sac1, sac1_length,
			&decoded.decision, expected, sizeof(expected), &expected_length))
		return 0;
	return length == expected_length && memcmp(wire, expected, length) == 0;
}

int sol_decision_trace_action_is_authorized_v1(const uint8_t *wire,
	size_t length, const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length)
{
	sol_decision_trace_v1 trace;
	sol_action_response_v1 action;
	uint8_t attack_bits;

	if (!sol_decision_trace_verify_v1(wire, length, sob1, sob1_length,
			sac1, sac1_length) ||
		!sol_decision_trace_decode_v1(wire, length, &trace) ||
		!sol_wire_decode_action_v1(sac1, sac1_length, &action) ||
		action.teamsay_present)
	{
		return 0;
	}
	attack_bits = action.buttons & UINT8_C(0x09);
	switch (trace.decision.decision_class)
	{
	case SOL_DECISION_CLASS_NEUTRAL_V1:
		return action.forwardmove == 0 && action.sidemove == 0 &&
			action.upmove == 0 && action.buttons == 0u &&
			action.weapon_select == SOL_WEAPON_KEEP;
	case SOL_DECISION_CLASS_EXPLORE_V1:
		return attack_bits == 0u;
	case SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1:
		return attack_bits != 0u;
	case SOL_DECISION_CLASS_RESPAWN_V1:
		return action.forwardmove == 0 && action.sidemove == 0 &&
			action.upmove == 0 && action.buttons == UINT8_C(0x01) &&
			action.weapon_select == SOL_WEAPON_KEEP;
	default:
		return 0;
	}
}
