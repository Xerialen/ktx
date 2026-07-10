if(NOT DEFINED NM OR NOT DEFINED ARCHIVE OR NOT DEFINED HEADER)
	message(FATAL_ERROR "NM, ARCHIVE, and HEADER are required")
endif()

file(STRINGS "${HEADER}" header_includes REGEX "^[ \t]*#[ \t]*include")
list(LENGTH header_includes include_count)
if(NOT include_count EQUAL 2)
	message(FATAL_ERROR "sol_core.h must include exactly stddef.h and stdint.h: ${header_includes}")
endif()
list(GET header_includes 0 first_include)
list(GET header_includes 1 second_include)
if(NOT first_include STREQUAL "#include <stddef.h>"
		OR NOT second_include STREQUAL "#include <stdint.h>")
	message(FATAL_ERROR "sol_core.h exposes a non-capability-free include: ${header_includes}")
endif()

execute_process(
	COMMAND "${NM}" -u "${ARCHIVE}"
	RESULT_VARIABLE nm_result
	OUTPUT_VARIABLE nm_output
	ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
	message(FATAL_ERROR "nm failed (${nm_result}): ${nm_error}")
endif()

set(forbidden_symbol
	"(g_edicts|g_globalvars|world|self|trap_[A-Za-z0-9_]*|cvar|cvar_string|g_random|find_plr|syscall|time|clock|gettimeofday)")
if(nm_output MATCHES "(^|[\r\n])[^\r\n]*[ \t]U[ \t]+${forbidden_symbol}([\r\n]|$)")
	message(FATAL_ERROR "sol_core imports a forbidden engine/world capability: ${CMAKE_MATCH_0}")
endif()

message(STATUS "sol_core capability denylist passed")
