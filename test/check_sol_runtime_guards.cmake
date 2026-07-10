function(require_occurrences path pattern minimum description)
	file(READ "${path}" contents)
	string(REGEX MATCHALL "${pattern}" matches "${contents}")
	list(LENGTH matches count)
	if(count LESS minimum)
		message(FATAL_ERROR
			"SOL legacy-hook guard missing: ${description}; expected >=${minimum}, found ${count} in ${path}")
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
