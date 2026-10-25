# RSDKPlatform-webOS.cmake
#
# Platform definition for the RSDKv5 decompilation, included by
# external/RSDKv5-Decompilation/CMakeLists.txt through its
# include(platforms/${PLATFORM}.cmake) line (the top-level CMakeLists.txt sets
# PLATFORM to a relative path that lands here), so the engine submodule needs
# no CMake changes.
#
# Runs in the engine's directory scope: RETRO_FILES is the engine source list.
# Expects these from the top-level project:
#   SWM_ROOT          repository root
#   SWM_GAME_DIR      Sonic Mania game source directory (for GameMain.h)
#   SWM_TARGET_WEBOS  ON when cross-compiling for the TV

# The SDL2 backend covers video, audio and input, and is what webOS provides.
set(RETRO_SUBSYSTEM "SDL2" CACHE STRING "The subsystem to use" FORCE)

# Our launcher (src/main.cpp) replaces the engine's main().
list(REMOVE_ITEM RETRO_FILES RSDKv5/main.cpp)
add_executable(RetroEngine ${RETRO_FILES} "${SWM_ROOT}/src/main.cpp")

target_compile_definitions(RetroEngine PRIVATE
    RETRO_WEBOS_PORT=1
    $<$<BOOL:${SWM_TARGET_WEBOS}>:SWM_TARGET_WEBOS=1>
)

target_include_directories(RetroEngine PRIVATE
    "${SWM_ROOT}/src"
    "${SWM_ROOT}/src/port"
    "${SWM_GAME_DIR}"
)

find_package(Threads REQUIRED)
# Plain (keyword-less) signature: the engine's own CMake links RetroEngine that way.
target_link_libraries(RetroEngine
    swm_port
    swm_theora
    ogg
    SDL2::SDL2
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(SWM_TARGET_WEBOS)
    # The TV's C++ runtime is older than the toolchain's; carry our own.
    target_link_options(RetroEngine PRIVATE -static-libstdc++ -static-libgcc)
    # Bundled libraries (SDL2) live in <app>/lib next to <app>/bin.
    # Only the relative path, so no build-machine paths end up in the binary.
    set_target_properties(RetroEngine PROPERTIES
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
endif()
