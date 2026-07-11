#ifndef SOL_DECISION_TRACE_H
#define SOL_DECISION_TRACE_H

#include <stddef.h>
#include <stdint.h>

#include "sol_sha256.h"

enum {
	SOL_DECISION_TRACE_BASE_SIZE_V1 = 79,
	SOL_DECISION_TRACE_TARGET_SIZE_V1 = 81
};

typedef enum sol_decision_class_v1 {
	SOL_DECISION_CLASS_NEUTRAL_V1 = 0,
	SOL_DECISION_CLASS_EXPLORE_V1 = 1,
	SOL_DECISION_CLASS_ENGAGE_VISIBLE_V1 = 2,
	SOL_DECISION_CLASS_RESPAWN_V1 = 3
} sol_decision_class_v1;

typedef enum sol_attack_gate_v1 {
	SOL_ATTACK_GATE_NONE_V1 = 0,
	SOL_ATTACK_GATE_RESPAWN_ONLY_V1 = 1,
	SOL_ATTACK_GATE_VISIBLE_LIVE_PLAYER_DIFFERENT_BOTTOM_COLOR_V1 = 2
} sol_attack_gate_v1;

typedef enum sol_selected_sighting_tag_v1 {
	SOL_SELECTED_SIGHTING_NONE_V1 = 0,
	SOL_SELECTED_SIGHTING_PRESENT_V1 = 1
} sol_selected_sighting_tag_v1;

typedef struct sol_decision_trace_decision_v1 {
	uint8_t decision_class;
	uint8_t attack_gate;
	uint8_t selected_sighting_tag;
	uint16_t selected_sighting_token;
} sol_decision_trace_decision_v1;

typedef struct sol_decision_trace_v1 {
	uint64_t observation_frame_seq;
	uint8_t sob1_sha256[SOL_SHA256_DIGEST_SIZE_V1];
	uint8_t sac1_sha256[SOL_SHA256_DIGEST_SIZE_V1];
	sol_decision_trace_decision_v1 decision;
} sol_decision_trace_v1;

int sol_decision_trace_decision_is_canonical_v1(
	const sol_decision_trace_decision_v1 *decision);
/* Validates and hashes the exact canonical SOB1/SAC1 byte strings. */
int sol_decision_trace_encode_v1(const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length,
	const sol_decision_trace_decision_v1 *decision,
	uint8_t *output, size_t capacity, size_t *output_length);
int sol_decision_trace_decode_v1(const uint8_t *wire, size_t length,
	sol_decision_trace_v1 *output);
/* Recomputes the canonical trace and requires an exact artifact binding. */
int sol_decision_trace_verify_v1(const uint8_t *wire, size_t length,
	const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length);
int sol_decision_trace_action_is_authorized_v1(const uint8_t *wire,
	size_t length, const uint8_t *sob1, size_t sob1_length,
	const uint8_t *sac1, size_t sac1_length);

#endif
