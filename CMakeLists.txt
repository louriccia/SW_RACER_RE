cmake_minimum_required(VERSION 3.16)
project(SW_RACER_RE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(USE_RELEASE_HOOK OFF)

if (MINGW)
    add_compile_options(
            -Wno-multichar
            -Werror=incompatible-pointer-types
            -Werror=implicit-function-declaration
            # x87-only float math like the original; clang's i686 default is SSE2 math
            -march=pentiumpro
            -mfpmath=387
    )
endif()

include(cmake/Sanitizers.cmake)

if (NOT PYTHON_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})
endif()

# automatically generate globals.h/globals.c if data_symbols.syms is updated

add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/global_touch.stamp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/globals.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/globals.c
    COMMAND ${CMAKE_COMMAND} -E touch global_touch.stamp
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/GenerateGlobalHeaderFromSymbols.py
    DEPENDS ${CMAKE_SOURCE_DIR}/data_symbols.syms
)
add_custom_target(generate_global_h ALL DEPENDS global_touch.stamp)

add_library(swe1r_globals INTERFACE)
target_include_directories(swe1r_globals INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(swe1r_globals INTERFACE INCLUDE_DX_HEADERS=1)

file(GLOB SOURCES
    # base files
    src/main.c
    src/swr.c
    src/stdPlatform.c
    src/FUN.c
    src/hook_mode.c
    # specifics
    src/Dss/*c
    src/Engine/*.c
    src/General/*.c
    src/Gui/*.c
    src/Main/*.c
    src/Platform/*.c
    src/Primitives/*.c
    src/Raster/*.c
    src/Unknown/*.c
    src/Win95/*.c
    src/Swr/*.c
)
add_library(swe1r_functions OBJECT ${SOURCES})
add_dependencies(swe1r_functions generate_global_h)
target_include_directories(swe1r_functions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(swe1r_functions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
target_compile_definitions(swe1r_functions PUBLIC GLFW_INCLUDE_NONE=1)
if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
    # Files checked bit-for-bit against retail by the verify harness. They round x87
    # intermediates on assignment and cast (ISO semantics), so a `float` local is a spill to
    # memory like MSVC's and `long double` stays at register width; without it GCC keeps floats
    # in registers at will and cannot match retail rounding. Floating literals are evaluated at
    # long double precision under this mode, so inexact ones must be written as exact hex floats.
    set_source_files_properties(
        src/General/stdMath.c
        src/Primitives/rdMath.c
        src/Primitives/rdMatrix.c
        src/Primitives/rdVector.c
        PROPERTIES COMPILE_OPTIONS -fexcess-precision=standard)
endif()
target_link_libraries(swe1r_functions PUBLIC swe1r_globals swr_memsafety winmm Comctl32 ole32 version glad glfw dxguid)

# based on $SOURCES, generate the hooked functions
set(HOOK_HEADER_FILES "")
file(GLOB_RECURSE HOOK_HEADER_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" "src/*.h")
message(STATUS "Using python located at " ${PYTHON_EXECUTABLE})
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/generated_hooks.stamp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/hook_generated.c
    COMMAND ${CMAKE_COMMAND} -E touch generated_hooks.stamp
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/GenerateHooks.py ${SOURCES} ${HOOK_HEADER_FILES}
    DEPENDS ${SOURCES}
)
add_custom_target(generate_hooks ALL DEPENDS generated_hooks.stamp)

if (USE_RELEASE_HOOK)
    # create our hook library
    file(GLOB HOOK_SOURCES
            src/hook.c
            src/generated/hook_generated.c
            src/dllMainDInput.c)
    add_library(dinput_hook SHARED ${HOOK_SOURCES})
    add_dependencies(dinput_hook generate_hooks)
    target_link_libraries(dinput_hook PRIVATE swe1r_functions swe1r_globals ddraw opengl32 dxguid)
    target_link_options(dinput_hook PRIVATE "-Wl,--kill-at")

    if (GAME_DIR)
        set_target_properties(dinput_hook PROPERTIES
                OUTPUT_NAME dinput
                PREFIX ""
                RUNTIME_OUTPUT_DIRECTORY "${GAME_DIR}")
    else()
        message(WARNING "Set 'GAME_DIR' to the directory of the game to automatically place 'dinput.dll' there when building.")
    endif()
endif()

add_subdirectory(dinput_hook)

# Standalone EXE: runs the game from src/ alone; retail's data sections come from the player's
# SWEP1RCR.EXE at runtime (standalone/standalone_main.c). Launch it with the game directory as cwd.
option(SWR_STANDALONE "Build swe1r_standalone.exe" OFF)
if (SWR_STANDALONE)
    set(STANDALONE_SOURCES ${SOURCES})
    list(FILTER STANDALONE_SOURCES EXCLUDE REGEX "(hook_mode\\.c|Platform/cstdlib\\.c)$")
    file(GLOB STANDALONE_OWN standalone/*.c standalone/*.cpp)
    list(APPEND STANDALONE_OWN ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook/lockstep.cpp)
    add_executable(swe1r_standalone WIN32
        ${STANDALONE_SOURCES}
        ${STANDALONE_OWN}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/hook_generated.c)
    add_dependencies(swe1r_standalone generate_global_h generate_hooks)
    target_include_directories(swe1r_standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook)
    target_compile_definitions(swe1r_standalone PRIVATE GLFW_INCLUDE_NONE=1 SWR_STANDALONE=1)
    target_link_libraries(swe1r_standalone PRIVATE swe1r_globals swr_memsafety winmm Comctl32 ole32 version dxguid dinput detours)
    # Keep the image clear of retail's 0x400000-0xED0000 range, which standalone_main.c maps.
    target_link_options(swe1r_standalone PRIVATE "-Wl,--image-base=0x10000000" "-Wl,--disable-dynamicbase" "-Wl,--stack,0x100000" "-static")

    # swe1r.exe: the standalone plus the dinput_hook feature layer (GL renderer, deltas, mods, UI),
    # attached to our own bodies by standalone/features/sa_features.cpp instead of to retail.
    file(GLOB SA_FEATURE_HOOK_SOURCES
        dinput_hook/*.c dinput_hook/*.cpp dinput_hook/game_deltas/*.c dinput_hook/game_deltas/*.cpp
        dinput_hook/camera/*.cpp dinput_hook/nv_dds/*.cpp)
    list(FILTER SA_FEATURE_HOOK_SOURCES EXCLUDE REGEX "dinput_hook/(main\.cpp|hook_helper\.cpp|dinput_hook\.c)$")
    set(SA_FEATURE_OWN ${STANDALONE_OWN})
    list(FILTER SA_FEATURE_OWN EXCLUDE REGEX "(standalone_lockstep\.cpp|dinput_hook/lockstep\.cpp)$")
    # Deltas attach at our function entries, so every call has to reach one: no inlining, and no
    # IPA clones (constprop/isra/part) or merged bodies (icf) that callers could reach instead. Five
    # NOPs at each entry give Detours room that no branch in the body targets.
    add_library(swe1r_game OBJECT ${STANDALONE_SOURCES})
    add_dependencies(swe1r_game generate_global_h generate_hooks)
    target_include_directories(swe1r_game PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone)
    target_compile_definitions(swe1r_game PRIVATE SWR_STANDALONE=1 SWR_STANDALONE_FEATURES=1)
    target_compile_options(swe1r_game PRIVATE -fno-inline -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra
        -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-modref -fno-ipa-ra -fpatchable-function-entry=5)
    target_link_libraries(swe1r_game PRIVATE swe1r_globals)
    add_executable(swe1r WIN32
        $<TARGET_OBJECTS:swe1r_game>
        ${SA_FEATURE_OWN}
        ${SA_FEATURE_HOOK_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone/features/sa_features.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/hook_generated.c)
    add_dependencies(swe1r generate_global_h generate_hooks generate_embedded_shaders generate_build_id)
    target_include_directories(swe1r PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook
        ${CMAKE_CURRENT_BINARY_DIR}/dinput_hook/generated)
    target_compile_definitions(swe1r PRIVATE GLFW_INCLUDE_NONE=1 SWR_STANDALONE=1 SWR_STANDALONE_FEATURES=1
        ENABLE_GLFW_INPUT_HANDLING=0 ENABLE_GAMEPAD_NAV=1 ENABLE_XINPUT_RUMBLE=1)
    target_link_libraries(swe1r PRIVATE swe1r_globals swr_memsafety imgui glad glfw fastgltf detours
        winmm Comctl32 ole32 version dxguid dinput ddraw opengl32 Dwmapi bcrypt kernel32)
    target_link_options(swe1r PRIVATE "-Wl,--image-base=0x10000000" "-Wl,--disable-dynamicbase" "-Wl,--stack,0x100000" "-static"
        "-Wl,--wrap=WinMain@16")
endif()
