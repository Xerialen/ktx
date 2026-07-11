#ifndef SOL_BRAIN_H
#define SOL_BRAIN_H

#include <stddef.h>
#include <stdint.h>

enum {
	SOL_BRAIN_TRACE_MAX_V1 = 81,
	SOL_BRAIN_STUCK_REPLAN_MIN_MS_V1 = 50,
	SOL_BRAIN_STUCK_REPLAN_MAX_MS_V1 = 5000,
	SOL_BRAIN_STUCK_REPLAN_DEFAULT_MS_V1 = 500
};

typedef struct sol_brain_v1 sol_brain_v1;

typedef struct sol_brain_bootstrap_v1 {
	uint32_t struct_size;
	uint32_t stuck_replan_ms;
	uint8_t static_asset_set_id[32];
	uint8_t sensory_profile_id[32];
} sol_brain_bootstrap_v1;

typedef enum sol_brain_status_v1 {
	SOL_BRAIN_DECISION = 1,
	SOL_BRAIN_NEUTRAL = 0,
	SOL_BRAIN_BAD_ARGUMENT = -1,
	SOL_BRAIN_BAD_OBSERVATION = -2,
	SOL_BRAIN_SEQUENCE_ERROR = -3,
	SOL_BRAIN_PROFILE_MISMATCH = -4,
	SOL_BRAIN_POISONED = -5,
	SOL_BRAIN_INTERNAL_ERROR = -6
} sol_brain_status_v1;

typedef struct sol_brain_decision_view_v1 {
	const uint8_t *sac1;
	size_t sac1_length;
	const uint8_t *sdt1;
	size_t sdt1_length;
} sol_brain_decision_view_v1;

sol_brain_v1 *sol_brain_open_v1(const sol_brain_bootstrap_v1 *bootstrap);

sol_brain_status_v1 sol_brain_decide_v1(sol_brain_v1 *brain,
	const uint8_t *sob1, size_t sob1_length,
	sol_brain_decision_view_v1 *decision);

void sol_brain_close_v1(sol_brain_v1 *brain);

#endif
