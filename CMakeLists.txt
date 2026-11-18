cmake_minimum_required(VERSION 3.20)
project(ShogoNative VERSION 0.1.0 LANGUAGES C CXX)

# Monolith's Shogo v2.2 game source (shogsrc22.zip). Not part of this repository:
# its EULA forbids redistribution, so each user supplies their own copy.
set(SHOGO_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/shogsrc22/source" CACHE PATH
    "Extracted Shogo v2.2 game source (contains AppHeaders/, ObjectDLL/, ClientShellDLL/)")
if(NOT EXISTS "${SHOGO_SOURCE_DIR}/AppHeaders/server_de.h")
    message(FATAL_ERROR "Shogo game source not found at ${SHOGO_SOURCE_DIR}. "
                        "Extract shogsrc22.zip there (see README) or set SHOGO_SOURCE_DIR.")
endif()

# Copy the pristine source into the build tree with our normalizations and patches.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SHOGO_PREPARED_DIR "${CMAKE_BINARY_DIR}/gamesrc")
file(GLOB SHOGO_PATCHES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/patches/*.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/prepare_source.py" ${SHOGO_PATCHES})
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/prepare_source.py"
            "${SHOGO_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/patches" "${SHOGO_PREPARED_DIR}"
    RESULT_VARIABLE _prep_result)
if(NOT _prep_result EQUAL 0)
    message(FATAL_ERROR "Preparing the Shogo game source failed")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Flags shared by everything compiled from Monolith's game code.
add_library(shogo_game_flags INTERFACE)
target_include_directories(shogo_game_flags INTERFACE
    "${CMAKE_SOURCE_DIR}/compat/include"
    "${SHOGO_PREPARED_DIR}/AppHeaders"
    "${SHOGO_PREPARED_DIR}/Shared")
target_compile_definitions(shogo_game_flags INTERFACE RIOT_BUILD NDEBUG SHOGO_NATIVE)
target_compile_options(shogo_game_flags INTERFACE
    "SHELL:-include ${CMAKE_SOURCE_DIR}/compat/include/shogo_platform.h"
    -fms-extensions
    -fno-strict-aliasing
    -fdelayed-template-parsing  # VC6-era templates reference types declared later
    -ferror-limit=0
    # The 1998 code is warning-heavy: silence warnings, but keep every
    # pointer<->integer truncation a hard error (this is a 64-bit build).
    -Wno-everything
    -Werror=microsoft-cast
    -Werror=pointer-to-int-cast
    -Werror=int-to-pointer-cast
    -Werror=void-pointer-to-int-cast)

# Small runtime pieces the game code expects from MSVCRT / Win32.
add_library(shogo_compat STATIC compat/src/msvcrt.c compat/src/ptrtoken.cpp compat/src/win32.cpp)
target_include_directories(shogo_compat PUBLIC "${CMAKE_SOURCE_DIR}/compat/include")
set_target_properties(shogo_compat PROPERTIES POSITION_INDEPENDENT_CODE ON)

# OBJECT.LTO: server-side game code (AI, weapons, world objects).
include(cmake/sources_object.cmake)
list(TRANSFORM SHOGO_OBJECT_SOURCES PREPEND "${SHOGO_PREPARED_DIR}/")
add_library(shogo_object MODULE ${SHOGO_OBJECT_SOURCES})
target_include_directories(shogo_object PRIVATE "${SHOGO_PREPARED_DIR}/ObjectDLL")
target_link_libraries(shogo_object PRIVATE shogo_game_flags shogo_compat)
set_target_properties(shogo_object PROPERTIES OUTPUT_NAME "object" PREFIX "" SUFFIX ".lto.dylib")

# CSHELL.DLL: client-side game code (HUD, menus, effects, input).
include(cmake/sources_cshell.cmake)
# NetStart.cpp is the Win32-dialog multiplayer wizard; replaced by our own src/game/NetStart_native.cpp.
# Win32-only glue files are replaced by our own native versions in src/game/:
#   NetStart.cpp   multiplayer start/join wizard (Win32 dialogs)
#   IpMgr.cpp      IP address list box, only used by the wizard
#   WinUtil.cpp    file/INI/timer helpers
#   KeyFixes.cpp   Windows keyboard-layout remapping
#   CreditsWin.cpp credits text read from CRES.DLL resources
list(REMOVE_ITEM SHOGO_CSHELL_SOURCES ClientShellDLL/NetStart.cpp ClientShellDLL/IpMgr.cpp
     ClientShellDLL/WinUtil.cpp ClientShellDLL/KeyFixes.cpp ClientShellDLL/CreditsWin.cpp)
list(TRANSFORM SHOGO_CSHELL_SOURCES PREPEND "${SHOGO_PREPARED_DIR}/")
list(APPEND SHOGO_CSHELL_SOURCES src/game/NetStart_native.cpp src/game/WinUtil_native.cpp
     src/game/KeyFixes_native.cpp src/game/CreditsWin_native.cpp)
add_library(shogo_cshell MODULE ${SHOGO_CSHELL_SOURCES})
target_include_directories(shogo_cshell PRIVATE "${SHOGO_PREPARED_DIR}/ClientShellDLL")
target_link_libraries(shogo_cshell PRIVATE shogo_game_flags shogo_compat)
set_target_properties(shogo_cshell PROPERTIES OUTPUT_NAME "cshell" PREFIX "" SUFFIX ".dylib")

# ---------------------------------------------------------------------------
# Stub engine: logging implementations of every ClientDE/ServerDE entry point,
# generated from Clang's AST of the API headers.
set(SHOGO_STUB_AST_TU "${CMAKE_SOURCE_DIR}/engine/api_ast_tu.cpp")
set(SHOGO_STUBS_CPP "${CMAKE_BINARY_DIR}/generated/engine_stubs.cpp")
set(_ast_flags -std=gnu++17 -fms-extensions -fsyntax-only -Wno-everything
    -include "${CMAKE_SOURCE_DIR}/compat/include/shogo_platform.h"
    -I "${CMAKE_SOURCE_DIR}/compat/include" -I "${CMAKE_SOURCE_DIR}"
    -I "${SHOGO_PREPARED_DIR}/AppHeaders" -I "${SHOGO_PREPARED_DIR}/Shared")
set(_ast_outputs)
foreach(_rec ClientDE ServerDE CSBase)
    set(_out "${CMAKE_BINARY_DIR}/generated/ast_${_rec}.json")
    add_custom_command(OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND "${CMAKE_CXX_COMPILER}" ${_ast_flags} -Xclang -ast-dump=json
                -Xclang -ast-dump-filter -Xclang ${_rec} "${SHOGO_STUB_AST_TU}" > "${_out}"
        DEPENDS "${SHOGO_STUB_AST_TU}" "${CMAKE_SOURCE_DIR}/engine/api.h"
                "${SHOGO_PREPARED_DIR}/AppHeaders/client_de.h" "${SHOGO_PREPARED_DIR}/AppHeaders/server_de.h"
                "${SHOGO_PREPARED_DIR}/AppHeaders/CSBase.h"
        COMMENT "Reading the ${_rec} interface from the API headers"
        VERBATIM)
    list(APPEND _ast_outputs "${_out}")
endforeach()
add_custom_command(OUTPUT "${SHOGO_STUBS_CPP}"
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/gen_engine_stubs.py" "${SHOGO_STUBS_CPP}" ${_ast_outputs}
    DEPENDS ${_ast_outputs} "${CMAKE_SOURCE_DIR}/tools/gen_engine_stubs.py"
    COMMENT "Generating engine API stubs"
    VERBATIM)

# The engine proper (grows as stubs are replaced with real implementations).
add_library(shogo_engine STATIC
    engine/rez_file.cpp engine/pe_resources.cpp engine/strings.cpp
    engine/surfaces.cpp engine/fonts_coretext.cpp engine/image_write_imageio.cpp engine/image_read_imageio.cpp engine/client_api.cpp engine/game_host.cpp
    engine/world_dat.cpp engine/dtx.cpp engine/render_world.cpp
    engine/math3d.cpp engine/api_common.cpp engine/messages.cpp engine/objects.cpp engine/server.cpp engine/savegame.cpp engine/client_game.cpp engine/interfaces.cpp engine/input.cpp engine/collision.cpp engine/movement.cpp
    engine/model.cpp
    engine/effects.cpp
    engine/sound.cpp
    engine/smacker.cpp engine/movie.cpp
    engine/dls.cpp engine/music.cpp
    engine/inno_setup.cpp engine/game_import.cpp
    engine/stub_support.cpp "${SHOGO_STUBS_CPP}")
target_include_directories(shogo_engine PUBLIC "${CMAKE_SOURCE_DIR}")
target_link_libraries(shogo_engine PUBLIC shogo_game_flags
    "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO"
    lzma z)

enable_testing()
add_executable(shogo-engine-tests engine/tests/engine_tests.cpp)
target_link_libraries(shogo-engine-tests PRIVATE shogo_engine)
add_test(NAME engine-tests COMMAND shogo-engine-tests)
file(GLOB SHOGO_TEST_INSTALLER "${CMAKE_SOURCE_DIR}/setup_shogo_*.exe")
set_tests_properties(engine-tests PROPERTIES ENVIRONMENT
    "SHOGO_DATA=${CMAKE_SOURCE_DIR}/gamedata;SHOGO_INSTALLER=${SHOGO_TEST_INSTALLER}")

# shogo-probe: loads both game modules against the stub engine and reports
# which engine calls the game makes during startup.
add_executable(shogo-probe engine/probe_main.cpp)
target_include_directories(shogo-probe PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(shogo-probe PRIVATE shogo_engine shogo_compat)
add_dependencies(shogo-probe shogo_object shogo_cshell)

# shogo: the game in a window.
find_package(SDL3 CONFIG REQUIRED)
add_executable(shogo engine/shogo_main.cpp engine/launcher.cpp engine/gpu_present.cpp)
target_include_directories(shogo PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(shogo PRIVATE shogo_engine shogo_compat SDL3::SDL3)
target_compile_definitions(shogo PRIVATE SHOGO_RECOMP_VERSION="${PROJECT_VERSION}")
add_dependencies(shogo shogo_object shogo_cshell)

# Shogo.app: the launcher and game, with the game modules and SDL3 inside.
add_custom_target(shogo-app ALL
    COMMAND "${CMAKE_COMMAND}" -DAPP=${CMAKE_BINARY_DIR}/Shogo.app -DEXE=$<TARGET_FILE:shogo>
            -DOBJECT=$<TARGET_FILE:shogo_object> -DCSHELL=$<TARGET_FILE:shogo_cshell>
            -DSDL=$<TARGET_FILE:SDL3::SDL3> -DVERSION=${PROJECT_VERSION}
            -P "${CMAKE_SOURCE_DIR}/cmake/make_app.cmake"
    DEPENDS shogo shogo_object shogo_cshell "${CMAKE_SOURCE_DIR}/cmake/make_app.cmake"
    COMMENT "Assembling Shogo.app"
    VERBATIM)

# shogo-worldinfo: parses every world and its textures (format check).
add_executable(shogo-worldinfo engine/worldinfo_main.cpp)
target_link_libraries(shogo-worldinfo PRIVATE shogo_engine)

# shogo-worldview: flies through a level with the world renderer.
add_executable(shogo-modelview engine/modelview_main.cpp)
target_link_libraries(shogo-modelview PRIVATE shogo_engine)

add_executable(shogo-worldview engine/worldview_main.cpp)
target_link_libraries(shogo-worldview PRIVATE shogo_engine SDL3::SDL3)

# shogo-music: renders music sections to a WAV file.
add_executable(shogo-music engine/music_main.cpp)
target_link_libraries(shogo-music PRIVATE shogo_engine)
