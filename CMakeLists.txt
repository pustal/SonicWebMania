# SonicWebMania: the Sonic Mania decompilation (RSDKv5) packaged as a native
# LG webOS TV app, plus a desktop Linux build of the same code for testing.
#
#   webOS:   cmake -B build-webos -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#            cmake --build build-webos && cmake --build build-webos --target webos-package
#   Desktop: cmake -B build && cmake --build build      (needs SDL2 development files)
#
# See README.md for the full walkthrough.

cmake_minimum_required(VERSION 3.21)
project(SonicWebMania VERSION 2.0.1 LANGUAGES C CXX)

set(SWM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${SWM_ROOT}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Target detection: the webosbrew toolchain's compiler reports "*-webos-*".
# ---------------------------------------------------------------------------
execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                OUTPUT_VARIABLE SWM_COMPILER_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(SWM_COMPILER_TARGET MATCHES "webos")
    set(SWM_TARGET_WEBOS ON)
else()
    set(SWM_TARGET_WEBOS OFF)
endif()
message(STATUS "SonicWebMania target: ${SWM_COMPILER_TARGET} (webOS: ${SWM_TARGET_WEBOS})")

set(SWM_APP_ID "org.sonicwebmania.app" CACHE STRING "webOS application id")
option(SWM_ENGINE_LOG "Let the engine write log.txt (off by default to spare TV storage)" OFF)

# ---------------------------------------------------------------------------
# Submodules must be present (and the engine patch applied).
# ---------------------------------------------------------------------------
set(SWM_RSDK_DIR "${SWM_ROOT}/external/RSDKv5-Decompilation")
set(SWM_MANIA_DIR "${SWM_ROOT}/external/Sonic-Mania-Decompilation")
foreach(required
        "${SWM_RSDK_DIR}/CMakeLists.txt"
        "${SWM_RSDK_DIR}/dependencies/all/tinyxml2/tinyxml2.cpp"
        "${SWM_MANIA_DIR}/SonicMania/Game.c"
        "${SWM_ROOT}/external/cpp-httplib/httplib.h"
        "${SWM_ROOT}/external/ogg/CMakeLists.txt"
        "${SWM_ROOT}/external/theora/lib/decode.c")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing ${required}. Run: git submodule update --init && "
                            "git -C external/RSDKv5-Decompilation submodule update --init")
    endif()
endforeach()

file(READ "${SWM_RSDK_DIR}/RSDKv5/RSDK/Graphics/SDL2/SDL2RenderDevice.cpp" _swm_render_device)
if(NOT _swm_render_device MATCHES "RETRO_WEBOS_PORT")
    message(FATAL_ERROR "The engine patch is not applied. Run: scripts/apply-patches.sh")
endif()

# ---------------------------------------------------------------------------
# SDL2: the bundled SDL-webOS backport on the TV, the system SDL2 on desktop.
# ---------------------------------------------------------------------------
if(SWM_TARGET_WEBOS)
    include(SDL2WebOSBackport)
else()
    find_package(SDL2 CONFIG QUIET)
    if(NOT TARGET SDL2::SDL2)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        add_library(SDL2::SDL2 ALIAS PkgConfig::SDL2)
    endif()
endif()

# ---------------------------------------------------------------------------
# Codecs for the game's FMV cutscenes (Theora video in Ogg), built statically
# so nothing depends on what a given firmware happens to ship.
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_PACKAGE_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ogg EXCLUDE_FROM_ALL)

# Theora has no CMake build; only its decoder is needed.
set(_swm_theora_dir "${SWM_ROOT}/external/theora")
add_library(swm_theora STATIC
    ${_swm_theora_dir}/lib/apiwrapper.c
    ${_swm_theora_dir}/lib/bitpack.c
    ${_swm_theora_dir}/lib/decapiwrapper.c
    ${_swm_theora_dir}/lib/decinfo.c
    ${_swm_theora_dir}/lib/decode.c
    ${_swm_theora_dir}/lib/dequant.c
    ${_swm_theora_dir}/lib/fragment.c
    ${_swm_theora_dir}/lib/huffdec.c
    ${_swm_theora_dir}/lib/idct.c
    ${_swm_theora_dir}/lib/info.c
    ${_swm_theora_dir}/lib/internal.c
    ${_swm_theora_dir}/lib/quant.c
    ${_swm_theora_dir}/lib/state.c
)
target_include_directories(swm_theora PUBLIC "${_swm_theora_dir}/include")
target_link_libraries(swm_theora PUBLIC ogg)
set_target_properties(swm_theora PROPERTIES C_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# Port layer: setup screen, transfer server, remote mapping, in-game menu.
# ---------------------------------------------------------------------------
set(SWM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${SWM_GENERATED_DIR}/WebPage.generated.h"
    COMMAND "${CMAKE_COMMAND}"
            -DINPUT=${SWM_ROOT}/web/index.html
            -DOUTPUT=${SWM_GENERATED_DIR}/WebPage.generated.h
            -DVARIABLE=kWebPageHtml
            -P "${SWM_ROOT}/cmake/EmbedTextFile.cmake"
    DEPENDS "${SWM_ROOT}/web/index.html" "${SWM_ROOT}/cmake/EmbedTextFile.cmake"
    COMMENT "Embedding the transfer web page"
)

add_library(swm_port STATIC
    src/port/BitmapFont.cpp
    src/port/Md5.cpp
    src/port/Achievements.h
    src/port/NetInfo.cpp
    src/port/PortConfig.cpp
    src/port/PortRuntime.cpp
    src/port/RemoteInput.cpp
    src/port/RetroAchievements.cpp
    src/port/RetroGames.cpp
    src/port/RsdkPack.cpp
    src/port/SetupScreen.cpp
    src/port/Storage.cpp
    src/port/TextRenderer.cpp
    src/port/TransferServer.cpp
    src/port/UsbDrives.cpp
    "${SWM_GENERATED_DIR}/WebPage.generated.h"
)
target_compile_features(swm_port PUBLIC cxx_std_17)
set_target_properties(swm_port PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(swm_port
    PUBLIC "${SWM_ROOT}/src/port"
    PRIVATE "${SWM_GENERATED_DIR}" "${SWM_ROOT}/external/cpp-httplib")
target_compile_definitions(swm_port
    PUBLIC SWM_VERSION="${PROJECT_VERSION}"
    PRIVATE $<$<BOOL:${SWM_TARGET_WEBOS}>:SWM_TARGET_WEBOS=1>)
target_compile_options(swm_port PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion>)
find_package(Threads REQUIRED)
target_link_libraries(swm_port PUBLIC SDL2::SDL2 Threads::Threads)

# ---------------------------------------------------------------------------
# Sonic Mania game logic, linked statically into the engine (GAME_STATIC).
# Mirrors external/Sonic-Mania-Decompilation/CMakeLists.txt, which would
# otherwise pull in its own copy of the engine.
# ---------------------------------------------------------------------------
set(GAME_NAME "SonicMania")
set(GAME_STATIC ON)
set(WITH_RSDK ON)
set(GAME_VERSION 6)
set(MANIA_PREPLUS OFF)
set(MANIA_FIRST_RELEASE OFF)
set(GAME_INCLUDE_EDITOR OFF)
set(SWM_GAME_DIR "${SWM_MANIA_DIR}/${GAME_NAME}")

add_library(${GAME_NAME} STATIC
    "${SWM_GAME_DIR}/Game.c"
    "${SWM_GAME_DIR}/Objects/All.c"
)
target_include_directories(${GAME_NAME} PRIVATE "${SWM_GAME_DIR}" "${SWM_GAME_DIR}/Objects")
set_target_properties(${GAME_NAME} PROPERTIES OUTPUT_NAME Game)

# ---------------------------------------------------------------------------
# The engine. Its CMakeLists includes platforms/${PLATFORM}.cmake; this
# relative path leads from its platforms/ folder to our platform file.
# ---------------------------------------------------------------------------
set(PLATFORM "../../../cmake/RSDKPlatform-webOS" CACHE STRING "RSDK platform file" FORCE)
set(RETRO_REVISION 3 CACHE STRING "RSDK revision (3 = v5U)" FORCE)
set(RETRO_MOD_LOADER OFF CACHE BOOL "No mod loader on the TV build" FORCE)
# Plus stays available to the engine; the port gates it behind an opt-in that
# is off by default (see WebOSPort_IsPlusEnabled), as the decomp licence requires.
set(RETRO_DISABLE_PLUS OFF CACHE BOOL "" FORCE)
if(SWM_ENGINE_LOG)
    set(RETRO_DISABLE_LOG OFF CACHE BOOL "" FORCE)
else()
    set(RETRO_DISABLE_LOG ON CACHE BOOL "" FORCE)
endif()
set(RETRO_OUTPUT_NAME "SonicWebMania" CACHE STRING "Executable name" FORCE)

add_subdirectory("${SWM_RSDK_DIR}" rsdk)

target_link_libraries(RetroEngine ${GAME_NAME})
# Our launcher and port headers use C++17.
set_target_properties(RetroEngine PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# webOS packaging: stage the IPK layout and run ares-package.
# ---------------------------------------------------------------------------
if(SWM_TARGET_WEBOS)
    include(WebOSPackage)
endif()
