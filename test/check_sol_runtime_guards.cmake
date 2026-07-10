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
	"(sol_core_step_v1|sol_ktx_encode_observation_v1|Sol_CapturePostThink|cov_get_committed_v1)"
	"legacy observation/core path or stack-sized GET payload")

file(READ "${SOL_RUNTIME}" sol_runtime_contents)
string(FIND "${sol_runtime_contents}" "sol_observation_client_poll_v1" poll_index)
string(FIND "${sol_runtime_contents}" "trap_SetBotCMD" command_index)
if(poll_index LESS 0 OR command_index LESS 0 OR poll_index GREATER command_index)
	message(FATAL_ERROR
		"SOL phase order must poll/seal observations before the first command side effect")
endif()
