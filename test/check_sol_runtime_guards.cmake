function(require_occurrences path pattern minimum description)
	file(READ "${path}" contents)
	string(REGEX MATCHALL "${pattern}" matches "${contents}")
	list(LENGTH matches count)
	if(count LESS minimum)
		message(FATAL_ERROR
			"SOL legacy-hook guard missing: ${description}; expected >=${minimum}, found ${count} in ${path}")
	endif()
endfunction()

function(forbid_occurrences path pattern description)
	file(READ "${path}" contents)
	if(contents MATCHES "${pattern}")
		message(FATAL_ERROR
			"SOL forbidden runtime capability present: ${description} in ${path}")
	endif()
endfunction()

function(require_exact_occurrences path pattern expected description)
	file(READ "${path}" contents)
	string(REGEX MATCHALL "${pattern}" matches "${contents}")
	list(LENGTH matches count)
	if(NOT count EQUAL expected)
		message(FATAL_ERROR
			"SOL exact guard failed: ${description}; expected ${expected}, found ${count} in ${path}")
	endif()
endfunction()

require_occurrences("${BOT_PHYS}" "!Sol_IsClient\\((p|self)\\)" 2
	"both global Frogbot pre-physics passes exclude SOL")
require_occurrences("${CLIENT}" "!Sol_IsClient\\(self\\)" 5
	"prethink, postthink, and all three bot-water paths exclude SOL")
require_occurrences("${ITEMS}" "!Sol_IsClient\\(player\\)" 1
	"legacy item-touch veto excludes SOL")
require_occurrences("${BOT_ENEMY}" "Sol_IsClient\\(targ\\)" 1
	"legacy damage reaction excludes SOL")
require_occurrences("${BOT_ENEMY}" "!Sol_IsClient\\(plr\\)" 1
	"legacy sound listeners exclude SOL")
require_occurrences("${COMBAT}" "!Sol_IsClient\\(targ\\)" 1
	"legacy air-acceleration bookkeeping excludes SOL")
require_occurrences("${BOT_COMMANDS}" "Sol_IsClient\\(self\\)" 1
	"per-client Frogbot StartFrame path excludes SOL")
require_occurrences("${BOT_CLIENT}" "Sol_Client(Connected|Enters)Event" 2
	"connect and enter lifecycle dispatch through SOL before Frogbot init")
require_occurrences("${BOT_CLIENT}" "Sol_IsClient\\(self\\)" 2
	"death cleanup retains legacy-opponent reset while excluding SOL-private death state")
require_occurrences("${BOT_STAT}" "Sol_IsClient\\(client\\)" 1
	"central Frogbot health/armor model excludes SOL")
require_occurrences("${BOT_HAZARD}" "Sol_IsClient\\((other|player)\\)" 2
	"legacy pre/post teleport hooks exclude SOL")
require_occurrences("${BOT_ITEMS}" "Sol_IsClient\\((player|self)\\)" 2
	"legacy item teamsay and dropped-backpack hooks exclude SOL")
require_occurrences("${BOT_MATCH}" "Sol_IsClient\\((p|p2|ent)\\)" 4
	"legacy team flags and match-start bot selection exclude SOL")
require_occurrences("${G_MAIN}" "!Sol_IsClient\\(self\\)" 2
	"pre/post-think wreg attack injection excludes SOL")
require_occurrences("${WEAPONS}" "!Sol_IsClient\\(self\\)" 1
	"weapon-frame wreg attack injection excludes SOL")
require_occurrences("${BOT_GOALS}" "Sol_IsClient\\(plr\\)" 1
	"item-taken goal refresh excludes the SOL controller while retaining legacy opponents")
require_occurrences("${MARKER_UTIL}" "Sol_IsClient\\(other\\)" 1
	"standalone Frogbot marker touches exclude SOL")

require_occurrences("${G_LOCAL}" "G_CONTROLLER_OBSERVATION_V1" 1
	"closed observer extension owns a mapped game-import slot")
require_occurrences("${G_MAIN}" "COV_EXTENSION_NAME_V1" 1
	"KTX maps the exact ControllerObservationV1 extension name")
require_occurrences("${G_SYSCALLS}" "trap_ControllerObservationV1" 1
	"native KTX exposes the closed observer syscall wrapper")
require_occurrences("${SOL_OBSERVATION_CLIENT}" "cov_get_committed_v1 get;" 1
	"the maximum native GET payload is owned by each heap client")
forbid_occurrences("${SOL_RUNTIME}"
	"(sol_core_step_v1|sol_ktx_encode_observation_v1|Sol_CapturePostThink|cov_get_committed_v1|\"SLO1\"|\"SLA1\")"
	"legacy observation/core path or stack-sized GET payload")
forbid_occurrences("${SOL_CANDIDATE_REGISTRY}"
	"(sol_core_step_v1|sol_ktx_encode_observation_v1|\"SLO1\"|\"SLA1\")"
	"old diagnostic core attached to the canonical SOB1 registry")

require_exact_occurrences("${SOL_RUNTIME}"
	"trap_ControllerEvidenceV1\\(CE_MATCH_BEGIN" 1
	"one run-level evidence begin call site")
require_exact_occurrences("${SOL_RUNTIME}"
	"trap_ControllerEvidenceV1\\(CE_MATCH_END" 1
	"one run-level evidence end call site")
require_exact_occurrences("${SOL_RUNTIME}"
	"trap_ControllerEvidenceV1\\(CE_FRAME_REQUEST" 1
	"one production frame-evidence writer")
require_exact_occurrences("${SOL_RUNTIME}" "trap_SetBotCMD\\(" 1
	"one production command writer")
require_occurrences("${SOL_RUNTIME}" "terminate_before_next_frame" 3
	"observation failure is surfaced and closes before a later bot frame")
require_exact_occurrences("${SOL_RUNTIME}"
	"sol_candidate_registry_remove_all_v1\\(" 1
	"one registry-wide production client-removal path")
require_occurrences("${SOL_CANDIDATE_REGISTRY}"
	"entries\\[SOL_KTX_CANDIDATE_COUNT_V1\\]" 1
	"the production registry owns the fixed four-entry table")
require_occurrences("${SOL_CANDIDATE_REGISTRY}"
	"sol_observation_client_v1 \\*observation" 1
	"every registry entry owns its private observation client")

file(READ "${SOL_CANDIDATE_REGISTRY}" sol_registry_contents)
string(FIND "${sol_registry_contents}"
	"poll_all_bound_v1(registry, dt_us, ops, prepared, results);" poll_index)
string(FIND "${sol_registry_contents}"
	"prepare_all_bound_v1(registry, msec, prepared);" prepare_index)
string(FIND "${sol_registry_contents}"
	"emit_all_bound_v1(registry, ops, prepared, results);" emit_index)
if(poll_index LESS 0 OR prepare_index LESS 0 OR emit_index LESS 0
		OR poll_index GREATER prepare_index OR prepare_index GREATER emit_index)
	message(FATAL_ERROR
		"SOL phase order must complete all polls, then all preparation, then all command side effects")
endif()

file(READ "${SOL_RUNTIME}" sol_runtime_contents)
string(FIND "${sol_runtime_contents}"
	"static void terminate_invalid_run(void)" terminate_function_index)
string(FIND "${sol_runtime_contents}"
	"close_evidence_run();\n\tremoved = remove_all_candidate_clients();" terminate_sequence_index)
string(FIND "${sol_runtime_contents}" "void Sol_StartFrame(void)" start_frame_index)
if(terminate_function_index LESS 0 OR terminate_sequence_index LESS terminate_function_index
		OR start_frame_index LESS terminate_sequence_index)
	message(FATAL_ERROR
		"invalid runs must close evidence and remove all SOL clients before returning to the next bot loop")
endif()
string(FIND "${sol_runtime_contents}"
	"identity = sol_ktx_plan_identity_v1(plan_seat);" identity_index)
string(FIND "${sol_runtime_contents}"
	"SOL evidencebind accepts candidate seats 1..4 only" control_reject_index)
string(FIND "${sol_runtime_contents}" "|| !ensure_registry()" registry_mutation_index)
if(identity_index LESS 0 OR control_reject_index LESS identity_index
		OR registry_mutation_index LESS control_reject_index)
	message(FATAL_ERROR
		"control seats 5..8 must be rejected before the SOL run epoch or registry can mutate")
endif()
