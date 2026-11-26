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
option(SWR_STANDALONE_DATA_RELOC "Standalone keeps retail data in its own image instead of at 0x4ac000" ON)
# swe1r: OpenAL Soft (OpenAL32.dll, loaded at runtime) instead of A3D.
option(SWR_STANDALONE_OPENAL "swe1r plays audio through OpenAL instead of A3D" ON)
option(SWR_STANDALONE_OWNED_GUARDED "Diagnostic: every owned global ends at a guard page (finds reads/writes past it)" OFF)
if (SWR_STANDALONE)
    set(STANDALONE_SOURCES ${SOURCES})

    # Owned globals: every named global with bytes of its own becomes a real variable
    # (standalone/tools/gen_owned_globals.py). Sizes come from the compiler, via a small program.
    set(OWNED_DIR ${CMAKE_CURRENT_BINARY_DIR}/owned)
    set(OWNED_SIZES_C ${OWNED_DIR}/global_sizes.c)
    add_custom_command(OUTPUT ${OWNED_SIZES_C}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OWNED_DIR}
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/standalone/tools/gen_owned_globals.py sizes
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/globals.h ${OWNED_SIZES_C}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/globals.h ${CMAKE_CURRENT_SOURCE_DIR}/standalone/tools/gen_owned_globals.py
        VERBATIM)
    add_executable(swr_global_sizes ${OWNED_SIZES_C})
    add_dependencies(swr_global_sizes generate_global_h)
    target_link_libraries(swr_global_sizes PRIVATE swe1r_globals)
    target_include_directories(swr_global_sizes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
    add_custom_command(OUTPUT ${OWNED_DIR}/globals_owned.h ${OWNED_DIR}/globals_owned.c
        COMMAND swr_global_sizes > ${OWNED_DIR}/global_sizes.txt
        COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/standalone/tools/gen_owned_globals.py owned
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/globals.h ${OWNED_DIR}/global_sizes.txt
                ${CMAKE_CURRENT_SOURCE_DIR}/standalone/owned_extents.txt
                ${OWNED_DIR}/globals_owned.h ${OWNED_DIR}/globals_owned.c $<$<BOOL:${SWR_STANDALONE_OWNED_GUARDED}>:guarded>
        DEPENDS swr_global_sizes ${CMAKE_CURRENT_SOURCE_DIR}/standalone/tools/gen_owned_globals.py
                ${CMAKE_CURRENT_SOURCE_DIR}/standalone/owned_extents.txt
        VERBATIM)
    add_custom_target(generate_owned_globals DEPENDS ${OWNED_DIR}/globals_owned.h ${OWNED_DIR}/globals_owned.c)
    # Keeps each canary right after its block (see gen_owned_globals.py).
    set_source_files_properties(${OWNED_DIR}/globals_owned.c PROPERTIES COMPILE_OPTIONS "-fno-toplevel-reorder;-fno-common")
    set(OWNED_DEFS $<$<BOOL:${SWR_STANDALONE_DATA_RELOC}>:SWR_GLOBALS_OWNED=1>)
    set(OWNED_SRC)
    if (SWR_STANDALONE_DATA_RELOC)
        set(OWNED_SRC ${OWNED_DIR}/globals_owned.c)
    endif()
    list(FILTER STANDALONE_SOURCES EXCLUDE REGEX "(hook_mode\\.c|Platform/cstdlib\\.c)$")
    file(GLOB STANDALONE_OWN standalone/*.c standalone/*.cpp)
    list(APPEND STANDALONE_OWN ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook/lockstep.cpp)
    add_executable(swe1r_standalone WIN32
        ${STANDALONE_SOURCES}
        ${STANDALONE_OWN}
        ${OWNED_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/hook_generated.c)
    add_dependencies(swe1r_standalone generate_global_h generate_hooks generate_owned_globals)
    target_include_directories(swe1r_standalone PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook
        ${OWNED_DIR})
    target_compile_definitions(swe1r_standalone PRIVATE GLFW_INCLUDE_NONE=1 SWR_STANDALONE=1 $<$<BOOL:${SWR_STANDALONE_DATA_RELOC}>:SWR_DATA_RELOC=1> ${OWNED_DEFS})
    target_link_libraries(swe1r_standalone PRIVATE swe1r_globals swr_memsafety winmm Comctl32 ole32 version dxguid dinput detours psapi)
    if (SWR_STANDALONE_DATA_RELOC)
        # Nothing depends on a fixed address any more (no retail code or data range): plain ASLR.
        target_link_options(swe1r_standalone PRIVATE "-Wl,--dynamicbase" "-Wl,--stack,0x100000" "-static")
    else()
        # Keep the image clear of retail's 0x400000-0xED0000 range, which standalone_main.c maps.
        target_link_options(swe1r_standalone PRIVATE "-Wl,--image-base=0x10000000" "-Wl,--disable-dynamicbase" "-Wl,--stack,0x100000" "-static")
    endif()

    # swe1r.exe: the standalone plus the dinput_hook feature layer (GL renderer, deltas, mods, UI),
    # attached to our own bodies by standalone/features/sa_features.cpp instead of to retail.
    file(GLOB SA_FEATURE_HOOK_SOURCES
        dinput_hook/*.c dinput_hook/*.cpp dinput_hook/game_deltas/*.c dinput_hook/game_deltas/*.cpp
        dinput_hook/camera/*.cpp dinput_hook/nv_dds/*.cpp)
    if (SWR_STANDALONE_OPENAL)
        list(APPEND SA_FEATURE_HOOK_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook/audio/openal_audio.cpp)
    endif()
    list(FILTER SA_FEATURE_HOOK_SOURCES EXCLUDE REGEX "dinput_hook/(main\.cpp|hook_helper\.cpp|dinput_hook\.c)$")
    set(SA_FEATURE_OWN ${STANDALONE_OWN})
    list(FILTER SA_FEATURE_OWN EXCLUDE REGEX "(standalone_lockstep\.cpp|dinput_hook/lockstep\.cpp)$")
    # Deltas attach at our function entries, so every call has to reach one: no inlining, and no
    # IPA clones (constprop/isra/part) or merged bodies (icf) that callers could reach instead. Five
    # NOPs at each entry give Detours room that no branch in the body targets.
    add_library(swe1r_game OBJECT ${STANDALONE_SOURCES})
    add_dependencies(swe1r_game generate_global_h generate_hooks generate_owned_globals)
    target_include_directories(swe1r_game PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone
        ${OWNED_DIR})
    target_compile_definitions(swe1r_game PRIVATE SWR_STANDALONE=1 SWR_STANDALONE_FEATURES=1 $<$<BOOL:${SWR_STANDALONE_DATA_RELOC}>:SWR_DATA_RELOC=1> ${OWNED_DEFS})
    target_compile_options(swe1r_game PRIVATE -fno-inline -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra
        -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-modref -fno-ipa-ra -fpatchable-function-entry=5)
    target_link_libraries(swe1r_game PRIVATE swe1r_globals)
    add_executable(swe1r WIN32
        $<TARGET_OBJECTS:swe1r_game>
        ${SA_FEATURE_OWN}
        ${SA_FEATURE_HOOK_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone/features/sa_features.cpp
        ${OWNED_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/hook_generated.c)
    add_dependencies(swe1r generate_global_h generate_hooks generate_embedded_shaders generate_build_id generate_owned_globals)
    target_include_directories(swe1r PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/standalone
        ${CMAKE_CURRENT_SOURCE_DIR}/dinput_hook
        ${CMAKE_CURRENT_BINARY_DIR}/dinput_hook/generated
        ${OWNED_DIR})
    target_compile_definitions(swe1r PRIVATE GLFW_INCLUDE_NONE=1 SWR_STANDALONE=1 SWR_STANDALONE_FEATURES=1 $<$<BOOL:${SWR_STANDALONE_DATA_RELOC}>:SWR_DATA_RELOC=1> ${OWNED_DEFS}
        ENABLE_GLFW_INPUT_HANDLING=0 ENABLE_GAMEPAD_NAV=1 ENABLE_XINPUT_RUMBLE=1
        $<$<BOOL:${SWR_STANDALONE_OPENAL}>:SWR_OPENAL_AUDIO=1>)
    target_link_libraries(swe1r PRIVATE swe1r_globals swr_memsafety imgui glad glfw fastgltf detours
        winmm Comctl32 ole32 version dxguid dinput ddraw opengl32 Dwmapi bcrypt psapi kernel32)
    target_link_options(swe1r PRIVATE "-Wl,--image-base=0x10000000" "-Wl,--disable-dynamicbase" "-Wl,--stack,0x100000" "-static"
        "-Wl,--wrap=WinMain@16")
endif()
