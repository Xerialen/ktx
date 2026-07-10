cmake_policy(SET CMP0007 NEW)

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
forbid_occurrences("${SOL_EVIDENCE_RUN}"
	"(sol_core_step_v1|sol_ktx_encode_observation_v1|\"SLO1\"|\"SLA1\")"
	"evidence lifecycle coupled to the old diagnostic core")
forbid_occurrences("${SOL_RUNTIME_SCHEDULE}"
	"(CE_MATCH_END|CE_UNBIND|trap_RemoveBot|trap_SetBotCMD|CE_FRAME_REQUEST|CE_FRAME_REPLACE)"
	"pure runtime scheduling policy performing a frame or cleanup side effect")

require_exact_occurrences("${SOL_EVIDENCE_RUN}"
	"CE_MATCH_BEGIN" 1
	"one run-level evidence begin call site")
require_exact_occurrences("${SOL_EVIDENCE_RUN}"
	"CE_MATCH_END" 1
	"one run-level evidence end call site")
require_exact_occurrences("${SOL_EVIDENCE_RUN}" "CE_UNBIND" 1
	"one safe evidence unbind call site")
require_exact_occurrences("${G_SYSCALLS}"
	"trap_ControllerEvidenceV1\\(operation" 1
	"one global actual-command evidence writer")
require_exact_occurrences("${G_SYSCALLS}" "syscall\\(G_SetBotCMD" 1
	"one raw engine command syscall provenance point")
require_exact_occurrences("${G_SYSCALLS_H}"
	"#define trap_ReplaceBotCMD trap_SetBotCMD" 1
	"QVM replacement commands reuse the existing SetBotCMD import")
require_exact_occurrences("${G_SYSCALLS}" "sol_actual_command_submit_v1\\(" 1
	"public command wrapper delegates exactly once to the global hook")
forbid_occurrences("${SOL_RUNTIME}" "(CE_FRAME_REQUEST|CE_FRAME_REPLACE)"
	"candidate-specific frame-evidence callback after global interception")
forbid_occurrences("${SOL_CANDIDATE_REGISTRY}"
	"(CE_FRAME_REQUEST|CE_FRAME_REPLACE|candidate_evidence)"
	"candidate registry owning evidence instead of command preparation")
require_exact_occurrences("${SOL_RUNTIME}" "trap_SetBotCMD\\(" 1
	"candidate commands use the public globally intercepted writer")
require_exact_occurrences("${BOT_MOVEMENT}" "trap_SetBotCMD\\(" 1
	"stock movement uses the public globally intercepted writer")
require_exact_occurrences("${BOT_MOVEMENT}" "trap_ReplaceBotCMD\\(" 1
	"stock movement owns one explicit blocked-replacement writer")
require_exact_occurrences("${BOT_BLOCKED}" "BotReplaceCommand\\(self\\)" 1
	"only the stock BotBlocked callback declares a replacement")
forbid_occurrences("${BOT_BLOCKED}" "BotSetCommand\\(self\\)"
	"BotBlocked disguising a replacement as an ordinary frame request")
require_exact_occurrences("${BOT_COMMANDS}" "trap_SetBotCMD\\(" 1
	"stock debug command uses the public globally intercepted writer")
require_exact_occurrences("${NANO_BRAIN}" "trap_SetBotCMD\\(" 2
	"both nano command routes use the public globally intercepted writer")
foreach(producer "${SOL_RUNTIME}" "${BOT_MOVEMENT}" "${BOT_COMMANDS}" "${NANO_BRAIN}")
	forbid_occurrences("${producer}" "G_SetBotCMD"
		"producer bypassing the one raw syscall provenance point")
endforeach()
forbid_occurrences("${SOL_RUNTIME}" "(CE_MATCH_END|CE_UNBIND)"
	"end or unbind operation outside the safe evidence lifecycle")
require_occurrences("${SOL_RUNTIME}" "sol_evidence_run_fail_stop_v1" 5
	"frame and setup failures mark deferred fail-stop without immediate teardown")
require_exact_occurrences("${SOL_RUNTIME}"
	"sol_evidence_run_server_cleanup_v1\\(" 1
	"one safe non-bot evidence cleanup path")
require_exact_occurrences("${SOL_RUNTIME}"
	"sol_runtime_schedule_decide_v1\\(" 2
	"both runtime frame phases use the shared scheduling policy")
require_exact_occurrences("${SOL_RUNTIME}"
	"sol_evidence_run_emissions_open_v1\\(" 3
	"runtime scheduling and the command hook distinguish emissions-open from active")
require_exact_occurrences("${SOL_RUNTIME}" "if \\(!schedule.run_candidates\\)" 1
	"bot frame obeys the candidate scheduling decision")
require_exact_occurrences("${SOL_RUNTIME}" "if \\(!schedule.run_cleanup\\)" 1
	"server frame obeys the cleanup scheduling decision")
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
string(FIND "${sol_runtime_contents}" "void Sol_ServerStartFrame(void)" server_start_index)
string(FIND "${sol_runtime_contents}"
	"sol_evidence_run_server_cleanup_v1(sol.evidence," safe_cleanup_index)
string(FIND "${sol_runtime_contents}" "void Sol_StartFrame(void)" bot_start_index)
if(server_start_index LESS 0 OR safe_cleanup_index LESS server_start_index
		OR bot_start_index LESS safe_cleanup_index)
	message(FATAL_ERROR
		"end, unbind, and removal must be owned by the non-bot server-frame hook")
endif()
string(FIND "${sol_runtime_contents}"
	"identity = sol_ktx_plan_identity_v1(plan_seat);" identity_index)
string(FIND "${sol_runtime_contents}"
	"SOL evidencebind accepts lifecycle seats 1..8." eight_seat_index)
string(FIND "${sol_runtime_contents}"
	"ControllerObservationV1 is unavailable for candidate seats." candidate_cov_index)
string(FIND "${sol_runtime_contents}" "|| !ensure_runtime()" runtime_mutation_index)
if(identity_index LESS 0 OR eight_seat_index LESS identity_index
		OR candidate_cov_index LESS eight_seat_index
		OR runtime_mutation_index LESS candidate_cov_index)
	message(FATAL_ERROR
		"eight-seat identity and candidate-only COV eligibility must precede runtime mutation")
endif()

string(FIND "${sol_runtime_contents}"
	"result = trap_ControllerEvidenceV1(CE_BIND" ce_bind_index)
string(FIND "${sol_runtime_contents}"
	"sol_evidence_run_record_bind_v1(sol.evidence" record_bind_index)
string(FIND "${sol_runtime_contents}"
	"strcmp(bind.observed_player_name" identity_validation_index)
string(FIND "${sol_runtime_contents}"
	"sol_candidate_registry_bind_v1(sol.candidates" observation_bind_index)
if(ce_bind_index LESS 0 OR record_bind_index LESS ce_bind_index
		OR identity_validation_index LESS record_bind_index
		OR observation_bind_index LESS identity_validation_index)
	message(FATAL_ERROR
		"every CE_BIND OK route must be retained before identity validation or COV creation")
endif()

require_occurrences("${SOL_KTX_ADAPTER_H}" "SOL_KTX_CANDIDATE_COUNT_V1 = 4" 1
	"candidate policy registry remains exactly four seats")
require_occurrences("${SOL_KTX_ADAPTER_H}" "SOL_KTX_EVIDENCE_SEAT_COUNT_V1 = 8" 1
	"generic evidence lifecycle owns exactly eight seats")
require_occurrences("${SOL_KTX_ADAPTER}" "control-[5-8]" 4
	"four canonical control evidence identities exist")
require_occurrences("${SOL_KTX_ADAPTER}" "ctrl-[5-8]" 4
	"four canonical stock-control player names exist")
require_occurrences("${SOL_KTX_ADAPTER}"
	"skill_level == 20 && !strcmp\\(team, \"blue\"\\)" 1
	"stock control selector is exactly addbot 20 blue")
require_occurrences("${SOL_LAUNCH_COORDINATOR}"
	"pending_index != SOL_KTX_EVIDENCE_SEAT_COUNT_V1" 1
	"generic coordinator permits exactly one launch-pending seat")
require_exact_occurrences("${BOT_COMMANDS}" "Sol_StockPendingBotName\\(" 1
	"stock addbot has one narrow SOL name override seam")
require_exact_occurrences("${BOT_COMMANDS}" "Sol_StockBotInitialized\\(" 1
	"stock addbot binds SOL only after ordinary initialization")
require_occurrences("${BOT_COMMANDS}" "FrogbotsRemoveBotByEntity" 1
	"control cleanup removes through the stock table by entity")
require_occurrences("${BOT_COMMANDS}" "FrogbotsForgetBotByEntity" 1
	"synchronous control disconnect clears the stock table by entity")
require_occurrences("${SOL_RUNTIME}" "FrogbotsRemoveBotByEntity" 1
	"control lifecycle cleanup uses the stock removal seam")
require_occurrences("${SOL_RUNTIME}" "FrogbotsForgetBotByEntity" 1
	"control disconnect uses the stock forget seam")
require_occurrences("${BOT_COMMANDS}" "if \\(Sol_RemoveAll\\(\\)\\)" 1
	"removeall defers stock-control removal to safe evidence cleanup")

file(READ "${BOT_COMMANDS}" bot_commands_contents)
string(FIND "${bot_commands_contents}"
	"Sol_StockPendingBotName(skill_level" stock_name_index)
string(FIND "${bot_commands_contents}"
	"entity = trap_AddBot(bots[i].name" stock_add_index)
string(FIND "${bot_commands_contents}"
	"trap_SetBotUserInfo(entity, \"*skill\"" stock_initialized_index)
string(FIND "${bot_commands_contents}"
	"Sol_StockBotInitialized(entity" stock_bind_index)
if(stock_name_index LESS 0 OR stock_add_index LESS stock_name_index
		OR stock_initialized_index LESS stock_add_index
		OR stock_bind_index LESS stock_initialized_index)
	message(FATAL_ERROR
		"control name override must precede stock add and CE bind must follow full stock initialization")
endif()

require_occurrences("${SOL_ACTUAL_COMMAND}"
	"input->msec < 1 \\|\\| input->msec > UINT8_MAX" 1
	"bound command evidence requires exact byte-representable msec")
require_occurrences("${SOL_ACTUAL_COMMAND}"
	"input->forwardmove < INT16_MIN \\|\\| input->forwardmove > INT16_MAX" 1
	"bound command evidence requires exact int16-representable movement")
forbid_occurrences("${SOL_ACTUAL_COMMAND}" "(bound\\(|clamp|normalize|trunc)"
	"global command hook normalizing or truncating an actual command")

file(READ "${SOL_ACTUAL_COMMAND}" sol_actual_command_contents)
string(FIND "${sol_actual_command_contents}"
	"evidence_result = ops->evidence" command_evidence_index)
string(FIND "${sol_actual_command_contents}"
	"actual_result = ops->actual" command_actual_index)
string(FIND "${sol_actual_command_contents}"
	"fail_after_actual(ops);" command_fail_index)
if(command_evidence_index LESS 0 OR command_actual_index LESS command_evidence_index
		OR command_fail_index LESS command_actual_index)
	message(FATAL_ERROR
		"canonical exact request must immediately precede actual command and fail-stop must follow it")
endif()

file(READ "${G_MAIN}" g_main_contents)
string(FIND "${g_main_contents}" "Sol_ServerStartFrame();" server_hook_index)
string(FIND "${g_main_contents}" "StartFrame(arg0);" ordinary_start_index)
if(server_hook_index LESS 0 OR ordinary_start_index LESS server_hook_index)
	message(FATAL_ERROR
		"non-bot SOL cleanup hook must run before ordinary StartFrame")
endif()
