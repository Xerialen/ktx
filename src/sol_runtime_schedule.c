#include "sol_runtime_schedule.h"

sol_runtime_schedule_decision_v1 sol_runtime_schedule_decide_v1(
	sol_runtime_frame_phase_v1 phase, int epoch_active,
	int registry_available, int cleanup_pending)
{
	sol_runtime_schedule_decision_v1 decision = { 0, 0 };

	if (!epoch_active)
	{
		return decision;
	}
	if (phase == SOL_RUNTIME_BOT_FRAME_V1)
	{
		/* Cleanup cannot pre-empt bot cadence before a server frame can own it. */
		decision.run_candidates = registry_available;
	}
	else if (phase == SOL_RUNTIME_SERVER_FRAME_V1)
	{
		decision.run_cleanup = cleanup_pending;
	}
	return decision;
}
