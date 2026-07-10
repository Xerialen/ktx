#ifndef SOL_RUNTIME_SCHEDULE_H
#define SOL_RUNTIME_SCHEDULE_H

typedef enum sol_runtime_frame_phase_v1
{
	SOL_RUNTIME_SERVER_FRAME_V1 = 0,
	SOL_RUNTIME_BOT_FRAME_V1 = 1
} sol_runtime_frame_phase_v1;

typedef struct sol_runtime_schedule_decision_v1
{
	int run_candidates;
	int run_cleanup;
} sol_runtime_schedule_decision_v1;

sol_runtime_schedule_decision_v1 sol_runtime_schedule_decide_v1(
	sol_runtime_frame_phase_v1 phase, int epoch_active,
	int registry_available, int cleanup_pending);

#endif
