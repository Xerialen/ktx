cmake_minimum_required(VERSION 3.10.0)

# Set project name and languge.
project(qwprogs C)

include(CheckCCompilerFlag)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# CMake option for bot support, we check it below and add C preprocessor directive if required.
option(BOT_SUPPORT "Build with bot support" ON)
# Option for FTE extensions. We also have some basic support for FTE which does not controlled by this setting.
# This is en example and extensions unused for now, so its turned off by default. Also turning it ON makes binary incompatibe with MVDSV.
option(FTESV "Build with FTE extensions support" OFF)
# nano-bots: rtx-port peer brain (qw-ctf/rtx bots). OFF by default; an OFF
# build is byte-identical to the stock frogbot baseline (no nano source
# compiled, no dispatch branch, no cvars, no struct change).
option(NANO_SUPPORT "Build with nano-bot (rtx-port) brain" OFF)
option(SOL_SUPPORT "Build the isolated SOL diagnostic core" OFF)
if(NANO_SUPPORT AND NOT BOT_SUPPORT)
	message(FATAL_ERROR "NANO_SUPPORT requires BOT_SUPPORT=ON (nano is a peer brain on the frogbot base).")
endif()
if(SOL_SUPPORT AND NOT BOT_SUPPORT)
	message(FATAL_ERROR "SOL_SUPPORT requires BOT_SUPPORT=ON (the native runtime uses bot lifecycle commands).")
endif()

# Set where sources located.
set(DIR_SRC "src")

# Set variables with different source lists for native or QVM library:

# g_syscalls.asm is a mapping between trap name and trap number, used by QVM library (it is q3asm code).
set(SRC_VM_SYSCALL "g_syscalls.asm")
# Sources for QVM library.
set(SRC_VM
	"${DIR_SRC}/bg_lib.c"
	)
# Sources for native library.
set(SRC_NATIVE
	"${DIR_SRC}/g_syscalls.c"
	"${DIR_SRC}/native_lib.c"
	)
# Common sources for native and QVM.
set(SRC_COMMON
	"${DIR_SRC}/g_main.c" # WARNING: should be first in list for QVM, since it contains vmMain().
	"${DIR_SRC}/admin.c"
	"${DIR_SRC}/arena.c"
	"${DIR_SRC}/bot_aim.c"
	"${DIR_SRC}/bot_blocked.c"
	"${DIR_SRC}/bot_botenemy.c"
	"${DIR_SRC}/bot_botgoals.c"
	"${DIR_SRC}/bot_bothazd.c"
	"${DIR_SRC}/bot_bothelp.c"
	"${DIR_SRC}/bot_botimp.c"
	"${DIR_SRC}/bot_botjump.c"
	"${DIR_SRC}/bot_botpath.c"
	"${DIR_SRC}/bot_botphys.c"
	"${DIR_SRC}/bot_botstat.c"
	"${DIR_SRC}/bot_botthink.c"
	"${DIR_SRC}/bot_botwater.c"
	"${DIR_SRC}/bot_botweap.c"
	"${DIR_SRC}/bot_client.c"
	"${DIR_SRC}/bot_commands.c"
	"${DIR_SRC}/bot_items.c"
	"${DIR_SRC}/bot_load.c"
	"${DIR_SRC}/bot_loadmap.c"
	"${DIR_SRC}/bot_match.c"
	"${DIR_SRC}/bot_movement.c"
	"${DIR_SRC}/bot_routing.c"
	"${DIR_SRC}/bot_world.c"
	"${DIR_SRC}/buttons.c"
	"${DIR_SRC}/captain.c"
	"${DIR_SRC}/coach.c"
	"${DIR_SRC}/clan_arena.c"
	"${DIR_SRC}/client.c"
	"${DIR_SRC}/combat.c"
	"${DIR_SRC}/commands.c"
	"${DIR_SRC}/ctf.c"
	"${DIR_SRC}/doors.c"
	"${DIR_SRC}/fb_globals.c"
	"${DIR_SRC}/files.c"
	"${DIR_SRC}/func_bob.c"
	"${DIR_SRC}/func_laser.c"
	"${DIR_SRC}/g_cmd.c"
	"${DIR_SRC}/globals.c"
	"${DIR_SRC}/g_mem.c"
	"${DIR_SRC}/grapple.c"
	"${DIR_SRC}/g_spawn.c"
	"${DIR_SRC}/g_userinfo.c"
	"${DIR_SRC}/g_utils.c"
	"${DIR_SRC}/g_syscalls_extra.c"
	"${DIR_SRC}/hiprot.c"
	"${DIR_SRC}/hoonymode.c"
	"${DIR_SRC}/items.c"
	"${DIR_SRC}/kbot_main.c"
	"${DIR_SRC}/kbot_gapjump.c"
	"${DIR_SRC}/kbot_dlog.c"
	"${DIR_SRC}/kbot_models.c"
	"${DIR_SRC}/kbot_harvest.c"
	"${DIR_SRC}/kbot_weapons.c"
	"${DIR_SRC}/kbot_dials.c"
	"${DIR_SRC}/logs.c"
	"${DIR_SRC}/maps.c"
	"${DIR_SRC}/maps_map_amphi2.c"
	"${DIR_SRC}/maps_map_dm3.c"
	"${DIR_SRC}/maps_map_dm4.c"
	"${DIR_SRC}/maps_map_dm6.c"
	"${DIR_SRC}/maps_map_povdmm4.c"
	"${DIR_SRC}/marker_load.c"
	"${DIR_SRC}/marker_util.c"
	"${DIR_SRC}/match.c"
	"${DIR_SRC}/mathlib.c"
	"${DIR_SRC}/misc.c"
	"${DIR_SRC}/motd.c"
	"${DIR_SRC}/plats.c"
	"${DIR_SRC}/player.c"
	"${DIR_SRC}/q_shared.c"
	"${DIR_SRC}/race.c"
	"${DIR_SRC}/rng.c"
	"${DIR_SRC}/rng_gen_impl.c"
	"${DIR_SRC}/rng_seed_impl.c"
	"${DIR_SRC}/route_calc.c"
	"${DIR_SRC}/route_fields.c"
	"${DIR_SRC}/route_lookup.c"
	"${DIR_SRC}/runes.c"
	"${DIR_SRC}/server.c"
	"${DIR_SRC}/sp_ai.c"
	"${DIR_SRC}/sp_boss.c"
	"${DIR_SRC}/sp_client.c"
	"${DIR_SRC}/sp_demon.c"
	"${DIR_SRC}/sp_dog.c"
	"${DIR_SRC}/spectate.c"
	"${DIR_SRC}/sp_enforcer.c"
	"${DIR_SRC}/sp_fish.c"
	"${DIR_SRC}/sp_hknight.c"
	"${DIR_SRC}/sp_knight.c"
	"${DIR_SRC}/sp_monsters.c"
	"${DIR_SRC}/sp_ogre.c"
	"${DIR_SRC}/sp_oldone.c"
	"${DIR_SRC}/sp_shalrath.c"
	"${DIR_SRC}/sp_shambler.c"
	"${DIR_SRC}/sp_soldier.c"
	"${DIR_SRC}/sp_tarbaby.c"
	"${DIR_SRC}/sp_wizard.c"
	"${DIR_SRC}/sp_zombie.c"
	"${DIR_SRC}/stats.c"
	"${DIR_SRC}/statsTables.c"
	"${DIR_SRC}/stats_json.c"
	"${DIR_SRC}/stats_xml.c"
	"${DIR_SRC}/subs.c"
	"${DIR_SRC}/teamplay.c"
	"${DIR_SRC}/triggers.c"
	"${DIR_SRC}/vip.c"
	"${DIR_SRC}/vote.c"
	"${DIR_SRC}/weapons.c"
	"${DIR_SRC}/world.c"
	)

# nano-bots (rtx port): compiled in only when NANO_SUPPORT is ON, so an OFF
# build stays byte-identical to the stock frogbot baseline.
if(NANO_SUPPORT)
	list(APPEND SRC_NATIVE
		"${DIR_SRC}/nano_main.c"
		"${DIR_SRC}/nano_bsp.c"
		"${DIR_SRC}/nano_navmesh.c"
		"${DIR_SRC}/nano_nav_build.c"
		"${DIR_SRC}/nano_brain.c")
endif()

if(SOL_SUPPORT)
	list(APPEND SRC_NATIVE "${DIR_SRC}/sol_runtime.c")
endif()

if(SOL_SUPPORT)
	add_library(sol_core STATIC "${DIR_SRC}/sol_core.c")
	add_library(sol_ktx_adapter STATIC "${DIR_SRC}/sol_ktx_adapter.c")
	add_library(sol_wire STATIC "${DIR_SRC}/sol_wire.c")
	target_include_directories(sol_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}")
	target_include_directories(sol_ktx_adapter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}")
	target_include_directories(sol_wire PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}")
	set_target_properties(sol_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
	set_target_properties(sol_ktx_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
	set_target_properties(sol_wire PROPERTIES POSITION_INDEPENDENT_CODE ON)
	if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
		target_compile_options(sol_core PRIVATE -Wall -Wextra -Werror -std=c17)
		target_compile_options(sol_ktx_adapter PRIVATE -Wall -Wextra -Werror -std=c17)
		target_compile_options(sol_wire PRIVATE -Wall -Wextra -Werror -std=c17)
		target_link_libraries(sol_core PUBLIC m)
	endif()
endif()

# Build native library.
add_library(${PROJECT_NAME} SHARED ${SRC_COMMON} ${SRC_NATIVE})
target_include_directories(${PROJECT_NAME} PRIVATE "include")
set_target_properties(${PROJECT_NAME}
	PROPERTIES PREFIX "" # Strip lib prefix.
	C_VISIBILITY_PRESET hidden # Hide all symbols unless excplicitly marked to export.
	)
if(BOT_SUPPORT)
	target_compile_definitions(${PROJECT_NAME} PRIVATE BOT_SUPPORT=1)
endif()
if(NANO_SUPPORT)
	target_compile_definitions(${PROJECT_NAME} PRIVATE NANO_SUPPORT=1)
endif()
if(SOL_SUPPORT)
	target_compile_definitions(${PROJECT_NAME} PRIVATE SOL_SUPPORT=1)
	target_link_libraries(${PROJECT_NAME} PRIVATE sol_core sol_ktx_adapter sol_wire)
endif()
if(FTESV)
	target_compile_definitions(${PROJECT_NAME} PRIVATE FTESV=1)
endif()

if(SOL_SUPPORT)
	include(CTest)
	if(BUILD_TESTING)
		add_executable(test_sol_core "test/test_sol_core.c")
		add_executable(test_sol_ktx_adapter "test/test_sol_ktx_adapter.c")
		add_executable(test_sol_wire "test/test_sol_wire.c")
		target_link_libraries(test_sol_core PRIVATE sol_core)
		target_link_libraries(test_sol_ktx_adapter PRIVATE sol_core sol_ktx_adapter)
		target_link_libraries(test_sol_wire PRIVATE sol_wire)
		if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
			target_compile_options(test_sol_core PRIVATE -Wall -Wextra -Werror -std=c17)
			target_compile_options(test_sol_ktx_adapter PRIVATE -Wall -Wextra -Werror -std=c17)
			target_compile_options(test_sol_wire PRIVATE -Wall -Wextra -Werror -std=c17)
		endif()
		add_test(NAME sol_core_contract COMMAND test_sol_core)
		add_test(NAME sol_ktx_adapter_contract COMMAND test_sol_ktx_adapter)
		add_test(NAME sol_wire_contract COMMAND test_sol_wire)
		add_test(NAME sol_runtime_guard_sentinel
			COMMAND ${CMAKE_COMMAND}
				"-DBOT_PHYS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_botphys.c"
				"-DCLIENT=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/client.c"
				"-DITEMS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/items.c"
				"-DBOT_ENEMY=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_botenemy.c"
				"-DCOMBAT=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/combat.c"
				"-DBOT_COMMANDS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_commands.c"
				"-DBOT_CLIENT=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_client.c"
				"-DBOT_STAT=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_botstat.c"
				"-DBOT_HAZARD=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_bothazd.c"
				"-DBOT_ITEMS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_items.c"
				"-DBOT_MATCH=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_match.c"
				"-DG_MAIN=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/g_main.c"
				"-DWEAPONS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/weapons.c"
				"-DBOT_GOALS=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/bot_botgoals.c"
				"-DMARKER_UTIL=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/marker_util.c"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_sol_runtime_guards.cmake")
		add_test(NAME sol_core_capability_denylist
			COMMAND ${CMAKE_COMMAND}
				"-DNM=${CMAKE_NM}"
				"-DARCHIVE=$<TARGET_FILE:sol_core>"
				"-DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/sol_core.h"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/test/check_sol_core_capabilities.cmake")
	endif()
endif()
# If not Miscrosoft compilator - then set additional options for linker.
if (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
	# 4100 - unused parameter
	# 4210 - using extern keyword in functions
	# 4456 - shadowing locals, broadly used
	# 4459 - shadowing globals, broadly used
	target_compile_options(${PROJECT_NAME} PRIVATE /W4 /wd4100 /wd4210 /wd4456 /wd4459)
else()
	target_compile_options(${PROJECT_NAME} PRIVATE -Wall -std=gnu17)

	check_c_compiler_flag("-Wstrict-prototypes" HAS_CFLAG_STRICT_PROTOTYPES)
	if (HAS_CFLAG_STRICT_PROTOTYPES)
		target_compile_options(${PROJECT_NAME} PRIVATE -Werror=strict-prototypes)
	endif()

	check_c_compiler_flag("-Wstrlcpy-strlcat-size" HAS_STRLCPY_STRLCAT_SIZE)
	if (HAS_STRLCPY_STRLCAT_SIZE)
		target_compile_options(${PROJECT_NAME} PRIVATE -Werror=strlcpy-strlcat-size)
	endif()

	# Do not allow undefined symbols while linking.
	if(APPLE)
		target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-undefined,error")
	else()
		target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--no-undefined")
	endif()
	target_link_libraries(${PROJECT_NAME} PRIVATE m) # Link with libm.
endif()


# Build QVM tools if needed.
find_program(Q3ASM q3asm)
if(Q3ASM)
	message(STATUS "q3asm found at ${Q3ASM}")
	set(QVM_LINKER "${Q3ASM}")
else()
	message(STATUS "q3asm not found, compiling bundled q3asm.")
	# EXCLUDE_FROM_ALL tells to not build tools if they does not required, for example if you build native library only.
	add_subdirectory(tools/q3asm EXCLUDE_FROM_ALL)
	set(QVM_LINKER "q3asm")
endif()

# Set up vars for QVM library.
set(VM_DIR "vm")
add_custom_target(vmdir
	COMMAND ${CMAKE_COMMAND} -E make_directory ${VM_DIR}
	)
set(QVM_COMPILER "q3lcc")
set(QVM_C_FLAGS -DQ3_VM -S -Wf-target=bytecode -Wf-g)
if(BOT_SUPPORT)
	set(QVM_C_FLAGS ${QVM_C_FLAGS} -DBOT_SUPPORT=1)
endif()
set(QVM_INCDIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Write first chunk of QVM_LIST_FILE.
set(QVM_LIST_FILE "${CMAKE_CURRENT_BINARY_DIR}/game.q3asm")
file(WRITE ${QVM_LIST_FILE} "-o ${PROJECT_NAME}\n")

# Cycle through sources and fill in SRC_ASM varible, continue generate QVM_LIST_FILE.
foreach(file ${SRC_COMMON} ${SRC_VM})
	get_filename_component(file_asm ${file} NAME_WE) # Get basename without extension.
	string(CONCAT file_asm ${file_asm} ".asm") # Add asm extension.
	list(APPEND SRC_ASM ${file_asm}) # Fill in SRC_ASM list. 
	file(APPEND ${QVM_LIST_FILE} "${VM_DIR}/${file_asm}\n") # Continue generate QVM_LIST_FILE.
	get_filename_component(src_dir ${file} DIRECTORY)
	file(RELATIVE_PATH QVM_RELINCDIR "${CMAKE_CURRENT_SOURCE_DIR}/${src_dir}" "${QVM_INCDIR}")
	# Add command to compile c to asm.
	add_custom_target(${file_asm}
		COMMAND ${QVM_COMPILER} ${QVM_C_FLAGS} -I${QVM_RELINCDIR} "${CMAKE_CURRENT_SOURCE_DIR}/${file}" -o "${VM_DIR}/${file_asm}"
		DEPENDS vmdir
		SOURCES ${file}
	)
endforeach()

# Copy file, but also add dependency.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${DIR_SRC}/${SRC_VM_SYSCALL} ${VM_DIR}/${SRC_VM_SYSCALL})
# Append last chunk of data in QVM_LIST_FILE.
file(APPEND ${QVM_LIST_FILE} "${VM_DIR}/${SRC_VM_SYSCALL}\n")
# Print location of the file.
message(STATUS ${QVM_LIST_FILE})
# Add custom target for processing asm files and generating QVM library.
add_custom_target(qvm
	COMMAND ${QVM_LINKER} -m -f ${QVM_LIST_FILE}
	DEPENDS ${SRC_ASM}
)
