#include "sol_brain.h"
#include "sol_decision_trace.h"
#include "sol_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char observation_golden_hex[] =
	"534f42310000000000000000c8320000"
	"571c7e6668b2bc1f259d60f2fd3217113e5b0694f8542cc2894a0984a8812ab0"
	"115cfbcfeb3635f4cd7b29782ff4157abacd0c1e634f6b7b40a48e6064669732"
	"01000000000000000001010000000000000002000000"
	"0000000000000000000100"
	"0101020100f8ff10001800640038ff0000000000400080b000"
	"640000003200000007000000190000001e000000050000000c000000"
	"002000402000000011000304000d04"
	"0100000000000000010101"
	"00000000000000000000";

static void require(int condition, const char *message)
{
	if (!condition)
	{
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static size_t hex_to_bytes(const char *hex, uint8_t *output, size_t capacity)
{
	size_t length = strlen(hex) / 2u;
	size_t index;

	require(strlen(hex) % 2u == 0u && length <= capacity,
		"fixture has a bounded even hex length");
	for (index = 0; index < length; ++index)
	{
		unsigned value;

		require(sscanf(hex + (index * 2u), "%2x", &value) == 1,
			"fixture hex parses");
		output[index] = (uint8_t) value;
	}
	return length;
}

static void fixture_u16(uint8_t *output, uint16_t value)
{
	output[0] = (uint8_t) value;
	output[1] = (uint8_t) (value >> 8);
}

static void fixture_u32(uint8_t *output, uint32_t value)
{
	output[0] = (uint8_t) value;
	output[1] = (uint8_t) (value >> 8);
	output[2] = (uint8_t) (value >> 16);
	output[3] = (uint8_t) (value >> 24);
}

static void fixture_u64(uint8_t *output, uint64_t value)
{
	fixture_u32(output, (uint32_t) value);
	fixture_u32(output + 4u, (uint32_t) (value >> 32));
}

static size_t append_visible_player(uint8_t observation[236],
	uint8_t top_color, uint8_t bottom_color)
{
	size_t offset = 198u;

	fixture_u32(observation + 194u, 1u);
	fixture_u16(observation + offset, 0u);
	offset += 2u;
	observation[offset++] = 0u;
	fixture_u16(observation + offset, 2u);
	offset += 2u;
	fixture_u16(observation + offset, 632u);
	fixture_u16(observation + offset + 2u, 16u);
	fixture_u16(observation + offset + 4u, 24u);
	offset += 6u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	fixture_u16(observation + offset, 0u);
	fixture_u16(observation + offset + 2u, 0u);
	fixture_u16(observation + offset + 4u, 0u);
	offset += 6u;
	fixture_u16(observation + offset, 0u);
	offset += 2u;
	observation[offset++] = top_color;
	observation[offset++] = bottom_color;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	observation[offset++] = 1u;
	observation[offset++] = 0u;
	memset(observation + offset, 0, 6u);
	offset += 6u;
	observation[offset++] = 0u;
	fixture_u32(observation + offset, 0u);
	offset += 4u;
	require(offset == 236u, "visible PLAYER fixture has exact SOB1 length");
	return offset;
}

static size_t append_visible_other_with_player_model(uint8_t observation[219])
{
	size_t offset = 198u;

	fixture_u32(observation + 194u, 1u);
	fixture_u16(observation + offset, 0u);
	offset += 2u;
	observation[offset++] = 3u;
	fixture_u16(observation + offset, 2u);
	offset += 2u;
	fixture_u16(observation + offset, 632u);
	fixture_u16(observation + offset + 2u, 16u);
	fixture_u16(observation + offset + 4u, 24u);
	offset += 6u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	observation[offset++] = 0u;
	fixture_u32(observation + offset, 0u);
	offset += 4u;
	require(offset == 219u, "visible OTHER fixture has exact SOB1 length");
	return offset;
}

static sol_brain_v1 *open_brain_for_observation(const uint8_t *observation)
{
	sol_brain_bootstrap_v1 bootstrap = {0};

	bootstrap.struct_size = sizeof(bootstrap);
	memcpy(bootstrap.static_asset_set_id, observation + 16u, 32u);
	memcpy(bootstrap.sensory_profile_id, observation + 48u, 32u);
	return sol_brain_open_v1(&bootstrap);
}

static sol_action_response_v1 require_exploration_without_fire(
	const sol_brain_decision_view_v1 *decision)
{
	sol_action_response_v1 action;

	require(decision != NULL && decision->sac1 != NULL &&
		sol_wire_decode_action_v1(decision->sac1, decision->sac1_length,
			&action),
		"non-target decision contains one canonical SAC1");
	require(action.view_angles[0] == 0.0f &&
		action.view_angles[1] == 90.0f &&
		action.view_angles[2] == -180.0f &&
		action.forwardmove == 400 && action.sidemove == 0 &&
		action.upmove == 0 && action.buttons == 0u &&
		action.weapon_select == SOL_WEAPON_KEEP &&
		!action.teamsay_present,
		"non-target sight preserves view and explores without firing");
	return action;
}

static void require_trace(const sol_brain_decision_view_v1 *decision,
	const uint8_t *observation, size_t observation_length,
	sol_decision_class_v1 decision_class, sol_attack_gate_v1 attack_gate,
	sol_selected_sighting_tag_v1 target_tag, uint16_t target_token)
{
	sol_decision_trace_v1 decoded;
	size_t expected_length = target_tag == SOL_SELECTED_SIGHTING_PRESENT_V1 ?
		SOL_DECISION_TRACE_TARGET_SIZE_V1 : SOL_DECISION_TRACE_BASE_SIZE_V1;

	require(decision && decision->sdt1 &&
		decision->sdt1_length == expected_length &&
		sol_decision_trace_decode_v1(decision->sdt1,
			decision->sdt1_length, &decoded) &&
		sol_decision_trace_verify_v1(decision->sdt1,
			decision->sdt1_length, observation, observation_length,
			decision->sac1, decision->sac1_length),
		"decision trace canonically binds the exact SOB1 and SAC1 bytes");
	require(decoded.decision.decision_class == decision_class &&
		decoded.decision.attack_gate == attack_gate &&
		decoded.decision.selected_sighting_tag == target_tag &&
		decoded.decision.selected_sighting_token == target_token,
		"decision trace carries the exact class, gate, and current sight token");
}

static void dirty_decision(sol_brain_decision_view_v1 *decision,
	const uint8_t *sentinel)
{
	decision->sac1 = sentinel;
	decision->sac1_length = 17u;
	decision->sdt1 = sentinel;
	decision->sdt1_length = 19u;
}

static void require_empty_decision(const sol_brain_decision_view_v1 *decision)
{
	require(decision->sac1 == NULL && decision->sac1_length == 0u &&
		decision->sdt1 == NULL && decision->sdt1_length == 0u,
		"failed decision exposes neither partial SAC1 nor partial SDT1");
}

static void require_failure_poison_cycle(const uint8_t *initial,
	size_t initial_length, const uint8_t *failure, size_t failure_length,
	sol_brain_status_v1 expected_failure, const uint8_t *would_be_next,
	size_t would_be_next_length, const char *failure_message)
{
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain = open_brain_for_observation(initial);

	require(brain != NULL, "failure fixture opens one private brain");
	require(sol_brain_decide_v1(brain, initial, initial_length, &decision) ==
		SOL_BRAIN_DECISION && decision.sac1 != NULL &&
		decision.sdt1 != NULL,
		"failure fixture first establishes one complete decision");
	dirty_decision(&decision, initial);
	require(sol_brain_decide_v1(brain, failure, failure_length, &decision) ==
		expected_failure, failure_message);
	require_empty_decision(&decision);
	dirty_decision(&decision, initial);
	require(sol_brain_decide_v1(brain, would_be_next,
		would_be_next_length, &decision) == SOL_BRAIN_POISONED,
		"any protocol failure permanently poisons the brain instance");
	require_empty_decision(&decision);
	sol_brain_close_v1(brain);
}

static void test_dead_self_requests_one_respawn_action(void)
{
	uint8_t observation[202];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_bootstrap_v1 bootstrap = {0};
	sol_brain_decision_view_v1 decision = {0};
	sol_action_response_v1 action;
	sol_brain_v1 *brain;

	bootstrap.struct_size = sizeof(bootstrap);
	memcpy(bootstrap.static_asset_set_id, observation + 16u, 32u);
	memcpy(bootstrap.sensory_profile_id, observation + 48u, 32u);
	observation[113] = 0u;
	observation[117] = 1u;
	brain = sol_brain_open_v1(&bootstrap);
	require(brain != NULL, "valid sealed bootstrap opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"dead self produces one active respawn decision");
	require(decision.sac1 != NULL &&
		sol_wire_decode_action_v1(decision.sac1, decision.sac1_length, &action),
		"respawn decision contains one canonical SAC1");
	require(action.frame_seq == 0u && action.view_angles[0] == 0.0f &&
		action.view_angles[1] == 90.0f && action.view_angles[2] == -180.0f,
		"respawn preserves the observer-authorized self view");
	require(action.forwardmove == 0 && action.sidemove == 0 &&
		action.upmove == 0 && action.buttons == 1u &&
		action.weapon_select == SOL_WEAPON_KEEP && !action.teamsay_present,
		"respawn is exactly ATTACK with no fabricated movement, weapon, or chat");
	require_trace(&decision, observation, observation_length,
		SOL_DECISION_CLASS_RESPAWN_V1, SOL_ATTACK_GATE_RESPAWN_ONLY_V1,
		SOL_SELECTED_SIGHTING_NONE_V1, 0u);
	sol_brain_close_v1(brain);
}

static void test_alive_self_without_player_sight_explores_without_firing(void)
{
	uint8_t observation[202];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_bootstrap_v1 bootstrap = {0};
	sol_brain_decision_view_v1 decision = {0};
	sol_action_response_v1 action;
	sol_brain_v1 *brain;

	bootstrap.struct_size = sizeof(bootstrap);
	memcpy(bootstrap.static_asset_set_id, observation + 16u, 32u);
	memcpy(bootstrap.sensory_profile_id, observation + 48u, 32u);
	brain = sol_brain_open_v1(&bootstrap);
	require(brain != NULL, "alive fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"alive self with no player sight produces active exploration");
	require(sol_wire_decode_action_v1(decision.sac1, decision.sac1_length,
		&action), "exploration decision contains one canonical SAC1");
	require(action.frame_seq == 0u && action.view_angles[0] == 0.0f &&
		action.view_angles[1] == 90.0f && action.view_angles[2] == -180.0f &&
		action.forwardmove == 400 && action.sidemove == 0 && action.upmove == 0,
		"initial exploration moves forward along the observed view");
	require(action.buttons == 0u && action.weapon_select == SOL_WEAPON_KEEP &&
		!action.teamsay_present,
		"exploration cannot fabricate fire, weapon selection, or chat");
	require_trace(&decision, observation, observation_length,
		SOL_DECISION_CLASS_EXPLORE_V1, SOL_ATTACK_GATE_NONE_V1,
		SOL_SELECTED_SIGHTING_NONE_V1, 0u);
	sol_brain_close_v1(brain);
}

static void test_locked_self_is_neutral_without_firing(void)
{
	uint8_t observation[202];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_action_response_v1 action;
	sol_brain_v1 *brain;

	observation[117] = 2u;
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length), "locked-self fixture is canonical SOB1");
	brain = open_brain_for_observation(observation);
	require(brain != NULL, "locked-self fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_NEUTRAL,
		"locked self produces an explicit neutral decision");
	require(sol_wire_decode_action_v1(decision.sac1, decision.sac1_length,
		&action) && action.frame_seq == 0u
		&& action.view_angles[0] == 0.0f
		&& action.view_angles[1] == 90.0f
		&& action.view_angles[2] == -180.0f
		&& action.forwardmove == 0 && action.sidemove == 0
		&& action.upmove == 0 && action.buttons == 0u
		&& action.weapon_select == SOL_WEAPON_KEEP
		&& !action.teamsay_present,
		"locked self preserves view and fabricates no action capability");
	require_trace(&decision, observation, observation_length,
		SOL_DECISION_CLASS_NEUTRAL_V1, SOL_ATTACK_GATE_NONE_V1,
		SOL_SELECTED_SIGHTING_NONE_V1, 0u);
	sol_brain_close_v1(brain);
}

static void test_visible_live_different_palette_player_authorizes_fire(void)
{
	uint8_t observation[236];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_bootstrap_v1 bootstrap = {0};
	sol_brain_decision_view_v1 decision = {0};
	sol_action_response_v1 action;
	sol_brain_v1 *brain;

	require(observation_length == 202u, "combat fixture starts canonical");
	observation[179] = 4u;
	observation[180] = 4u;
	observation_length = append_visible_player(observation, 13u, 13u);
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length), "different-palette PLAYER fixture is canonical SOB1");
	bootstrap.struct_size = sizeof(bootstrap);
	memcpy(bootstrap.static_asset_set_id, observation + 16u, 32u);
	memcpy(bootstrap.sensory_profile_id, observation + 48u, 32u);
	brain = sol_brain_open_v1(&bootstrap);
	require(brain != NULL, "combat fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"visible live different-palette PLAYER produces an active decision");
	require(sol_wire_decode_action_v1(decision.sac1, decision.sac1_length,
		&action), "engagement contains one canonical SAC1");
	require(action.buttons == 1u && action.forwardmove == 400 &&
		action.view_angles[1] == 0.0f && action.view_angles[0] > 10.0f &&
		action.view_angles[0] < 15.0f,
		"engagement aims from self eye to visible player center and attacks");
	require(action.weapon_select == SOL_WEAPON_KEEP && !action.teamsay_present,
		"first engagement neither fabricates weapon state nor communicates");
	require_trace(&decision, observation, observation_length,
		SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1,
		SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1,
		SOL_SELECTED_SIGHTING_PRESENT_V1, 0u);
	sol_brain_close_v1(brain);
}

static void test_visible_same_bottom_color_player_does_not_authorize_fire(void)
{
	uint8_t observation[236];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain;

	require(observation_length == 202u,
		"same-palette fixture starts canonical");
	observation[179] = 4u;
	observation[180] = 4u;
	observation_length = append_visible_player(observation, 13u, 4u);
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length), "same-bottom-color PLAYER fixture is canonical");
	brain = open_brain_for_observation(observation);
	require(brain != NULL, "same-palette fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"same-bottom-color PLAYER still permits active exploration");
	(void) require_exploration_without_fire(&decision);
	sol_brain_close_v1(brain);
}

static void test_visible_dead_player_does_not_authorize_fire(void)
{
	uint8_t observation[236];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain;

	require(observation_length == 202u, "dead PLAYER fixture starts canonical");
	observation[179] = 4u;
	observation[180] = 4u;
	observation_length = append_visible_player(observation, 13u, 13u);
	observation[221] = 1u;
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length), "dead PLAYER fixture is canonical");
	brain = open_brain_for_observation(observation);
	require(brain != NULL, "dead PLAYER fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"dead PLAYER still permits active exploration");
	(void) require_exploration_without_fire(&decision);
	sol_brain_close_v1(brain);
}

static void test_visible_gib_player_does_not_authorize_fire(void)
{
	uint8_t observation[236];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain;

	require(observation_length == 202u, "gib PLAYER fixture starts canonical");
	observation[179] = 4u;
	observation[180] = 4u;
	observation_length = append_visible_player(observation, 13u, 13u);
	observation[222] = 1u;
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length), "gib PLAYER fixture is canonical");
	brain = open_brain_for_observation(observation);
	require(brain != NULL, "gib PLAYER fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"gib PLAYER still permits active exploration");
	(void) require_exploration_without_fire(&decision);
	sol_brain_close_v1(brain);
}

static void test_visible_other_with_player_model_does_not_authorize_fire(void)
{
	uint8_t observation[219];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain;

	require(observation_length == 202u,
		"OTHER-with-player-model fixture starts canonical");
	observation[179] = 4u;
	observation[180] = 4u;
	observation_length = append_visible_other_with_player_model(observation);
	require(sol_wire_observation_is_canonical_v1(observation,
		observation_length),
		"OTHER rendered kind with player model token is canonical");
	brain = open_brain_for_observation(observation);
	require(brain != NULL,
		"OTHER-with-player-model fixture opens one private brain");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_DECISION,
		"rendered kind, not model token, controls target authorization");
	(void) require_exploration_without_fire(&decision);
	sol_brain_close_v1(brain);
}

static void test_target_disappearance_clears_fire_on_next_contiguous_frame(void)
{
	uint8_t visible[236];
	uint8_t disappeared[202];
	size_t visible_length = hex_to_bytes(observation_golden_hex,
		visible, sizeof(visible));
	size_t disappeared_length = hex_to_bytes(observation_golden_hex,
		disappeared, sizeof(disappeared));
	sol_brain_decision_view_v1 decision = {0};
	sol_action_response_v1 action;
	sol_brain_v1 *brain;

	require(visible_length == 202u && disappeared_length == 202u,
		"target-loss fixtures start canonical");
	visible[179] = 4u;
	visible[180] = 4u;
	visible_length = append_visible_player(visible, 13u, 13u);
	fixture_u64(disappeared + 4u, 1u);
	require(sol_wire_observation_is_canonical_v1(visible, visible_length) &&
		sol_wire_observation_is_canonical_v1(disappeared,
			disappeared_length),
		"target-loss fixtures are consecutive canonical SOB1 batches");
	brain = open_brain_for_observation(visible);
	require(brain != NULL, "target-loss fixture opens one private brain");
	require(sol_brain_decide_v1(brain, visible, visible_length, &decision) ==
		SOL_BRAIN_DECISION &&
		sol_wire_decode_action_v1(decision.sac1, decision.sac1_length,
			&action) && action.frame_seq == 0u && action.buttons == 1u,
		"visible target authorizes fire on frame zero");
	require(sol_brain_decide_v1(brain, disappeared, disappeared_length,
		&decision) == SOL_BRAIN_DECISION,
		"target disappearance still permits frame-one exploration");
	action = require_exploration_without_fire(&decision);
	require(action.frame_seq == 1u,
		"fire clears on the immediate next contiguous observation frame");
	sol_brain_close_v1(brain);
}

static void test_malformed_observation_clears_output_and_poisons(void)
{
	uint8_t initial[202];
	uint8_t would_be_next[202];
	uint8_t malformed[203];
	size_t initial_length = hex_to_bytes(observation_golden_hex,
		initial, sizeof(initial));
	size_t next_length = hex_to_bytes(observation_golden_hex,
		would_be_next, sizeof(would_be_next));

	require(initial_length == 202u && next_length == 202u,
		"malformed-observation fixtures start canonical");
	fixture_u64(would_be_next + 4u, 1u);
	memcpy(malformed, would_be_next, next_length);
	malformed[next_length] = 0xa5u;
	require(sol_wire_observation_is_canonical_v1(initial, initial_length) &&
		sol_wire_observation_is_canonical_v1(would_be_next, next_length) &&
		!sol_wire_observation_is_canonical_v1(malformed,
			next_length + 1u),
		"one trailing byte makes the otherwise-next batch malformed");
	require_failure_poison_cycle(initial, initial_length, malformed,
		next_length + 1u, SOL_BRAIN_BAD_OBSERVATION, would_be_next,
		next_length, "malformed SOB1 returns BAD_OBSERVATION");
}

static void test_profile_mismatch_clears_output_and_poisons(void)
{
	uint8_t initial[202];
	uint8_t would_be_next[202];
	uint8_t wrong_profile[202];
	size_t initial_length = hex_to_bytes(observation_golden_hex,
		initial, sizeof(initial));
	size_t next_length = hex_to_bytes(observation_golden_hex,
		would_be_next, sizeof(would_be_next));

	require(initial_length == 202u && next_length == 202u,
		"profile-mismatch fixtures start canonical");
	fixture_u64(would_be_next + 4u, 1u);
	memcpy(wrong_profile, would_be_next, next_length);
	wrong_profile[48] ^= 0x80u;
	require(sol_wire_observation_is_canonical_v1(wrong_profile, next_length),
		"profile-mismatch batch remains canonical SOB1");
	require_failure_poison_cycle(initial, initial_length, wrong_profile,
		next_length, SOL_BRAIN_PROFILE_MISMATCH, would_be_next, next_length,
		"sealed sensory-profile mismatch fails closed");
}

static void test_sequence_error_clears_output_and_poisons(void)
{
	uint8_t initial[202];
	uint8_t would_be_next[202];
	size_t initial_length = hex_to_bytes(observation_golden_hex,
		initial, sizeof(initial));
	size_t next_length = hex_to_bytes(observation_golden_hex,
		would_be_next, sizeof(would_be_next));

	require(initial_length == 202u && next_length == 202u,
		"sequence-error fixtures start canonical");
	fixture_u64(would_be_next + 4u, 1u);
	require(sol_wire_observation_is_canonical_v1(would_be_next, next_length),
		"would-be recovery frame is canonical and contiguous");
	require_failure_poison_cycle(initial, initial_length, initial,
		initial_length, SOL_BRAIN_SEQUENCE_ERROR, would_be_next, next_length,
		"duplicate observation frame returns SEQUENCE_ERROR");
}

static void test_bad_caller_arguments_poison_a_live_brain(void)
{
	uint8_t observation[202];
	size_t observation_length = hex_to_bytes(observation_golden_hex,
		observation, sizeof(observation));
	sol_brain_decision_view_v1 decision = {0};
	sol_brain_v1 *brain = open_brain_for_observation(observation);

	require(brain != NULL, "bad-argument fixture opens one private brain");
	dirty_decision(&decision, observation);
	require(sol_brain_decide_v1(brain, NULL, observation_length, &decision) ==
		SOL_BRAIN_BAD_ARGUMENT,
		"null SOB1 is a negative caller failure for a live brain");
	require_empty_decision(&decision);
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_POISONED,
		"caller failure permanently poisons the live brain");
	sol_brain_close_v1(brain);

	brain = open_brain_for_observation(observation);
	require(brain != NULL && sol_brain_decide_v1(brain, observation,
		observation_length, NULL) == SOL_BRAIN_BAD_ARGUMENT,
		"null decision view is a negative caller failure");
	require(sol_brain_decide_v1(brain, observation, observation_length,
		&decision) == SOL_BRAIN_POISONED,
		"null output failure also permanently poisons the brain");
	require_empty_decision(&decision);
	sol_brain_close_v1(brain);

	require(sol_brain_decide_v1(NULL, observation, observation_length,
		&decision) == SOL_BRAIN_BAD_ARGUMENT,
		"null brain reports bad argument without dereference");
	require_empty_decision(&decision);
}

int main(void)
{
	test_dead_self_requests_one_respawn_action();
	test_alive_self_without_player_sight_explores_without_firing();
	test_locked_self_is_neutral_without_firing();
	test_visible_live_different_palette_player_authorizes_fire();
	test_visible_same_bottom_color_player_does_not_authorize_fire();
	test_visible_dead_player_does_not_authorize_fire();
	test_visible_gib_player_does_not_authorize_fire();
	test_visible_other_with_player_model_does_not_authorize_fire();
	test_target_disappearance_clears_fire_on_next_contiguous_frame();
	test_malformed_observation_clears_output_and_poisons();
	test_profile_mismatch_clears_output_and_poisons();
	test_sequence_error_clears_output_and_poisons();
	test_bad_caller_arguments_poison_a_live_brain();
	printf("sol_brain: 13 contract tests passed\n");
	return 0;
}
