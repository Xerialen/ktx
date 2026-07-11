#include "sol_decision_trace.h"
#include "sol_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char observation_golden_hex[] =
	"534f4231efcdab8967452301c8320000"
	"571c7e6668b2bc1f259d60f2fd3217113e5b0694f8542cc2894a0984a8812ab0"
	"115cfbcfeb3635f4cd7b29782ff4157abacd0c1e634f6b7b40a48e6064669732"
	"01000000000000000001010000000000000002000000"
	"0000000000000000000100"
	"0101020100f8ff10001800640038ff0000000000400080b000"
	"640000003200000007000000190000001e000000050000000c000000"
	"002000402000000011000304000d04"
	"0100000000000000010101"
	"00000000000000000000";

static const char explore_trace_golden_hex[] =
	"53445431efcdab8967452301"
	"672e8f789b02b64fdf6535fec3871800aa61a691680dc6a12283bf7632268f9e"
	"6639989c9fcc97c301443a51506d33016869de53017a6597173045b027070c73"
	"010000";

static const char engage_trace_golden_hex[] =
	"53445431efcdab8967452301"
	"672e8f789b02b64fdf6535fec3871800aa61a691680dc6a12283bf7632268f9e"
	"6639989c9fcc97c301443a51506d33016869de53017a6597173045b027070c73"
	"0202013412";

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
		"decision-trace fixture has a bounded even hex length");
	for (i = 0; i < length; ++i) {
		unsigned value;

		require(sscanf(hex + (i * 2u), "%2x", &value) == 1,
			"decision-trace fixture hex parses");
		output[i] = (uint8_t)value;
	}
	return length;
}

static void fixture_u64(uint8_t *output, uint64_t value)
{
	unsigned i;

	for (i = 0; i < 8u; ++i)
		output[i] = (uint8_t)(value >> (i * 8u));
}

static void build_artifacts(uint8_t observation[202], uint8_t action[33])
{
	sol_action_response_v1 decoded;

	require(hex_to_bytes(observation_golden_hex, observation, 202u) == 202u,
		"canonical SOB1 fixture is exactly 202 bytes");
	memset(action, 0, 33u);
	memcpy(action, "SAC1", 4u);
	fixture_u64(action + 4u, UINT64_C(0x0123456789abcdef));
	require(sol_wire_observation_is_canonical_v1(observation, 202u),
		"independent SOB1 fixture is canonical");
	require(sol_wire_decode_action_v1(action, 33u, &decoded),
		"independent SAC1 fixture is canonical");
	require(decoded.frame_seq == UINT64_C(0x0123456789abcdef),
		"independent artifacts carry the intended frame sequence");
}

static int bytes_are_value(const uint8_t *bytes, size_t length, uint8_t value)
{
	size_t i;

	for (i = 0; i < length; ++i) {
		if (bytes[i] != value)
			return 0;
	}
	return 1;
}

static void test_exact_explore_encode_decode_and_verify(void)
{
	uint8_t observation[202], action[33], output[82], expected[79];
	sol_decision_trace_decision_v1 decision = {
		SOL_DECISION_CLASS_EXPLORE_V1,
		SOL_ATTACK_GATE_NONE_V1,
		SOL_SELECTED_SIGHTING_NONE_V1,
		0u
	};
	sol_decision_trace_v1 decoded;
	size_t output_length = 999u;

	build_artifacts(observation, action);
	require(hex_to_bytes(explore_trace_golden_hex, expected,
		sizeof(expected)) == sizeof(expected),
		"independent explore SDT1 fixture is exactly 79 bytes");
	memset(output, 0xa5, sizeof(output));
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, output, sizeof(output),
		&output_length), "canonical explore decision encodes");
	require(output_length == SOL_DECISION_TRACE_BASE_SIZE_V1,
		"no-target SDT1 uses the exact 79-byte form");
	require(memcmp(output, expected, sizeof(expected)) == 0,
		"explore SDT1 matches an independently hashed golden byte string");
	require(bytes_are_value(output + output_length,
		sizeof(output) - output_length, 0xa5),
		"SDT1 encoder writes no bytes beyond its declared length");
	require(sol_decision_trace_decode_v1(output, output_length, &decoded),
		"golden explore SDT1 decodes");
	require(decoded.observation_frame_seq == UINT64_C(0x0123456789abcdef) &&
		decoded.decision.decision_class == SOL_DECISION_CLASS_EXPLORE_V1 &&
		decoded.decision.attack_gate == SOL_ATTACK_GATE_NONE_V1 &&
		decoded.decision.selected_sighting_tag ==
			SOL_SELECTED_SIGHTING_NONE_V1 &&
		decoded.decision.selected_sighting_token == 0u,
		"decoded explore SDT1 preserves its canonical fields");
	require(sol_decision_trace_verify_v1(output, output_length,
		observation, sizeof(observation), action, sizeof(action)),
		"golden explore SDT1 verifies against the exact artifacts");
}

static void test_exact_engage_target_form(void)
{
	uint8_t observation[202], action[33], output[81], expected[81];
	sol_decision_trace_decision_v1 decision = {
		SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1,
		SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1,
		SOL_SELECTED_SIGHTING_PRESENT_V1,
		UINT16_C(0x1234)
	};
	sol_decision_trace_v1 decoded;
	size_t output_length = 0u;

	build_artifacts(observation, action);
	require(hex_to_bytes(engage_trace_golden_hex, expected,
		sizeof(expected)) == sizeof(expected),
		"independent engage SDT1 fixture is exactly 81 bytes");
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, output, sizeof(output),
		&output_length), "canonical engage decision encodes");
	require(output_length == SOL_DECISION_TRACE_TARGET_SIZE_V1,
		"target-bearing SDT1 uses the exact 81-byte form");
	require(memcmp(output, expected, sizeof(expected)) == 0,
		"engage SDT1 matches an independently hashed golden byte string");
	require(sol_decision_trace_decode_v1(output, output_length, &decoded),
		"golden engage SDT1 decodes");
	require(decoded.decision.decision_class ==
			SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1 &&
		decoded.decision.attack_gate ==
			SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1 &&
		decoded.decision.selected_sighting_tag ==
			SOL_SELECTED_SIGHTING_PRESENT_V1 &&
		decoded.decision.selected_sighting_token == UINT16_C(0x1234),
		"decoded engage SDT1 preserves its target token little-endian");
}

static void test_canonical_decision_relation_matrix(void)
{
	sol_decision_trace_decision_v1 decision;
	unsigned decision_class, attack_gate, target_tag;

	for (decision_class = 0u; decision_class <= 3u; ++decision_class) {
		for (attack_gate = 0u; attack_gate <= 2u; ++attack_gate) {
			for (target_tag = 0u; target_tag <= 1u; ++target_tag) {
				int expected =
					((decision_class == SOL_DECISION_CLASS_NEUTRAL_V1 ||
						decision_class == SOL_DECISION_CLASS_EXPLORE_V1) &&
						attack_gate == SOL_ATTACK_GATE_NONE_V1 &&
						target_tag == SOL_SELECTED_SIGHTING_NONE_V1) ||
					(decision_class == SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1 &&
						attack_gate ==
							SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1 &&
						target_tag == SOL_SELECTED_SIGHTING_PRESENT_V1) ||
					(decision_class == SOL_DECISION_CLASS_RESPAWN_V1 &&
						attack_gate == SOL_ATTACK_GATE_RESPAWN_ONLY_V1 &&
						target_tag == SOL_SELECTED_SIGHTING_NONE_V1);

				decision.decision_class = (uint8_t)decision_class;
				decision.attack_gate = (uint8_t)attack_gate;
				decision.selected_sighting_tag = (uint8_t)target_tag;
				decision.selected_sighting_token = target_tag ? 7u : 0u;
				require(sol_decision_trace_decision_is_canonical_v1(&decision) ==
					expected, "decision class/gate/target relation is exact");
			}
		}
	}
	decision.decision_class = 4u;
	decision.attack_gate = SOL_ATTACK_GATE_NONE_V1;
	decision.selected_sighting_tag = SOL_SELECTED_SIGHTING_NONE_V1;
	decision.selected_sighting_token = 0u;
	require(!sol_decision_trace_decision_is_canonical_v1(&decision),
		"unknown decision class is not canonical");
	decision.decision_class = SOL_DECISION_CLASS_EXPLORE_V1;
	decision.attack_gate = 3u;
	require(!sol_decision_trace_decision_is_canonical_v1(&decision),
		"unknown attack gate is not canonical");
	decision.attack_gate = SOL_ATTACK_GATE_NONE_V1;
	decision.selected_sighting_tag = 2u;
	require(!sol_decision_trace_decision_is_canonical_v1(&decision),
		"unknown target tag is not canonical");
	decision.selected_sighting_tag = SOL_SELECTED_SIGHTING_NONE_V1;
	decision.selected_sighting_token = 1u;
	require(!sol_decision_trace_decision_is_canonical_v1(&decision),
		"absent target has exactly one canonical in-memory representation");
	require(!sol_decision_trace_decision_is_canonical_v1(NULL),
		"null decision is not canonical");
}

static void test_decoder_rejects_noncanonical_envelopes_without_output(void)
{
	uint8_t base[81], target[81], corrupt[82];
	sol_decision_trace_v1 decoded;
	uint8_t zero[sizeof(decoded)];

	memset(zero, 0, sizeof(zero));
	require(hex_to_bytes(explore_trace_golden_hex, base, sizeof(base)) == 79u,
		"malformed tests start from exact base SDT1");
	require(hex_to_bytes(engage_trace_golden_hex, target, sizeof(target)) == 81u,
		"malformed tests start from exact target SDT1");
	memcpy(corrupt, base, 79u);
	corrupt[0] ^= 1u;
	memset(&decoded, 0xa5, sizeof(decoded));
	require(!sol_decision_trace_decode_v1(corrupt, 79u, &decoded) &&
		memcmp(&decoded, zero, sizeof(zero)) == 0,
		"bad SDT1 magic rejects without partial decoded output");
	require(!sol_decision_trace_decode_v1(base, 78u, &decoded),
		"truncated no-target SDT1 rejects");
	memcpy(corrupt, base, 79u);
	corrupt[79] = 0u;
	require(!sol_decision_trace_decode_v1(corrupt, 80u, &decoded),
		"extended no-target SDT1 rejects");
	memcpy(corrupt, target, 81u);
	corrupt[78] = SOL_SELECTED_SIGHTING_NONE_V1;
	require(!sol_decision_trace_decode_v1(corrupt, 81u, &decoded),
		"no-target tag cannot retain target-token bytes");
	memcpy(corrupt, base, 79u);
	corrupt[78] = SOL_SELECTED_SIGHTING_PRESENT_V1;
	require(!sol_decision_trace_decode_v1(corrupt, 79u, &decoded),
		"target tag requires both token bytes");
	memcpy(corrupt, base, 79u);
	corrupt[76] = SOL_DECISION_CLASS_RESPAWN_V1;
	require(!sol_decision_trace_decode_v1(corrupt, 79u, &decoded),
		"decoder enforces class/gate relation");
	require(!sol_decision_trace_decode_v1(base, 79u, NULL),
		"decoder rejects null output");
	require(!sol_decision_trace_decode_v1(NULL, 79u, &decoded),
		"decoder rejects null wire");
}

static void test_encoder_and_verifier_fail_closed(void)
{
	uint8_t observation[202], action[34], output[81], trace[79];
	uint8_t original_output[81];
	sol_action_response_v1 decoded_action;
	sol_decision_trace_v1 decoded;
	sol_decision_trace_decision_v1 decision = {
		SOL_DECISION_CLASS_EXPLORE_V1,
		SOL_ATTACK_GATE_NONE_V1,
		SOL_SELECTED_SIGHTING_NONE_V1,
		0u
	};
	size_t output_length;

	build_artifacts(observation, action);
	require(hex_to_bytes(explore_trace_golden_hex, trace, sizeof(trace)) ==
		sizeof(trace), "verification tests start from exact SDT1");
	require(sol_decision_trace_verify_v1(trace, sizeof(trace), observation,
		sizeof(observation), action, 33u), "exact artifact binding verifies");
	trace[12] ^= 1u;
	require(sol_decision_trace_decode_v1(trace, sizeof(trace), &decoded),
		"a changed digest remains structurally decodable");
	require(!sol_decision_trace_verify_v1(trace, sizeof(trace), observation,
		sizeof(observation), action, 33u),
		"verifier rejects an altered SOB1 digest");
	trace[12] ^= 1u;
	action[24] ^= 1u;
	require(sol_wire_decode_action_v1(action, 33u, &decoded_action),
		"SAC1 byte mutation remains canonical for exact-hash testing");
	require(!sol_decision_trace_verify_v1(trace, sizeof(trace), observation,
		sizeof(observation), action, 33u),
		"verifier hashes every exact SAC1 byte");
	action[24] ^= 1u;
	observation[12] ^= 1u;
	require(sol_wire_observation_is_canonical_v1(observation,
		sizeof(observation)),
		"SOB1 byte mutation remains canonical for exact-hash testing");
	require(!sol_decision_trace_verify_v1(trace, sizeof(trace), observation,
		sizeof(observation), action, 33u),
		"verifier hashes every exact SOB1 byte");
	observation[12] ^= 1u;
	fixture_u64(action + 4u, UINT64_C(0x0123456789abcdee));
	memset(output, 0xa5, sizeof(output));
	memcpy(original_output, output, sizeof(output));
	output_length = 999u;
	require(!sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, 33u, &decision, output, sizeof(output), &output_length) &&
		output_length == 0u &&
		memcmp(output, original_output, sizeof(output)) == 0,
		"frame mismatch rejects without partial SDT1 output");
	fixture_u64(action + 4u, UINT64_C(0x0123456789abcdef));
	output_length = 999u;
	require(!sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, 33u, &decision, output, 78u, &output_length) &&
		output_length == 0u &&
		memcmp(output, original_output, sizeof(output)) == 0,
		"short capacity rejects without partial SDT1 output");
	require(!sol_decision_trace_encode_v1(NULL, sizeof(observation), action,
		33u, &decision, output, sizeof(output), &output_length),
		"encoder rejects null SOB1 bytes");
	require(!sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, 33u, NULL, output, sizeof(output), &output_length),
		"encoder rejects null decision");
	require(!sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, 33u, &decision, output, sizeof(output), NULL),
		"encoder requires a length result");
	action[33] = 0u;
	require(!sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, 34u, &decision, output, sizeof(output), &output_length),
		"encoder rejects a noncanonical SAC1 with trailing bytes");
}

static void test_action_semantics_are_closed_by_decision_class(void)
{
	uint8_t observation[202], action[33], trace[81];
	size_t trace_length = 0u;
	sol_decision_trace_decision_v1 decision = {
		SOL_DECISION_CLASS_EXPLORE_V1, SOL_ATTACK_GATE_NONE_V1,
		SOL_SELECTED_SIGHTING_NONE_V1, 0u
	};

	build_artifacts(observation, action);
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"stationary explore intent is valid when it fabricates no attack");
	action[30] = 1u;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		!sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"explore intent cannot wrap a hash-correct attack action");

	action[30] = 0u;
	action[24] = 0x90u;
	action[25] = 0x01u;
	decision.decision_class = SOL_DECISION_CLASS_NEUTRAL_V1;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		!sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"neutral intent cannot conceal movement");

	action[24] = 0u;
	action[25] = 0u;
	decision.decision_class = SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1;
	decision.attack_gate =
		SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1;
	decision.selected_sighting_tag = SOL_SELECTED_SIGHTING_PRESENT_V1;
	decision.selected_sighting_token = 7u;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		!sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"engage-visible intent must actually request an attack");
	action[30] = 1u;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"engage-visible attack with a target-bearing gate is action-valid");

	decision.decision_class = SOL_DECISION_CLASS_RESPAWN_V1;
	decision.attack_gate = SOL_ATTACK_GATE_RESPAWN_ONLY_V1;
	decision.selected_sighting_tag = SOL_SELECTED_SIGHTING_NONE_V1;
	decision.selected_sighting_token = 0u;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"respawn intent permits exactly a movement-free ordinary attack");
	action[31] = 7u;
	require(sol_decision_trace_encode_v1(observation, sizeof(observation),
		action, sizeof(action), &decision, trace, sizeof(trace), &trace_length) &&
		!sol_decision_trace_action_is_authorized_v1(trace, trace_length,
			observation, sizeof(observation), action, sizeof(action)),
		"respawn intent cannot select a weapon");
}

int main(void)
{
	test_exact_explore_encode_decode_and_verify();
	test_exact_engage_target_form();
	test_canonical_decision_relation_matrix();
	test_decoder_rejects_noncanonical_envelopes_without_output();
	test_encoder_and_verifier_fail_closed();
	test_action_semantics_are_closed_by_decision_class();
	printf("sol_decision_trace: 6 contract tests passed\n");
	return 0;
}
