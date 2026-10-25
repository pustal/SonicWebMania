/**
 * @file main.cpp
 * @brief Entry point of the webOS build: prepares storage, starts the transfer
 *        server, shows the setup screen when no game data is installed, then
 *        hands over to the RSDKv5 engine running Sonic Mania.
 *
 * Replaces RSDKv5/main.cpp from the engine (excluded in cmake/RSDKPlatform-webOS.cmake).
 */
#include "RSDK/Core/RetroEngine.hpp"

// Sonic Mania is linked statically (GAME_STATIC), so its logic entry point
// comes from the game's GameMain.h rather than from a loaded library.
// GameMain.h expects the unqualified engine type name, as in RSDKv5/main.cpp.
#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#undef EngineInfo

#include "port/NetInfo.h"
#include "port/PortConfig.h"
#include "port/PortRuntime.h"
#include "port/RemoteInput.h"
#include "port/SetupScreen.h"
#include "port/Storage.h"
#include "port/TransferServer.h"

#include <unistd.h>

#include <cstdio>
#include <fstream>

namespace
{

/**
 * Engine settings suited to a TV, written only on first launch (the engine
 * rewrites Settings.ini itself whenever the player changes options).
 */
constexpr const char *kDefaultSettings = "[Game]\n"
                                         "devMenu=n\n"
                                         "faceButtonFlip=n\n"
                                         "\n"
                                         "[Video]\n"
                                         "windowed=n\n"
                                         "border=n\n"
                                         "exclusiveFS=n\n"
                                         "vsync=y\n"
                                         "tripleBuffering=n\n"
                                         "pixWidth=424\n"
                                         "winWidth=1280\n"
                                         "winHeight=720\n"
                                         "refreshRate=60\n"
                                         "screenShader=0\n"
                                         "\n"
                                         "[Audio]\n"
                                         "streamsEnabled=y\n"
                                         "streamVolume=0.8\n"
                                         "sfxVolume=1.0\n";

/** Writes kDefaultSettings unless the player already has a Settings.ini. */
void WriteDefaultSettingsIfMissing(const swm::Storage &storage)
{
    const std::string path = storage.PathFor("Settings.ini");
    if (swm::Storage::FileSize(path) > 0)
        return;

    std::ofstream out(path, std::ios::trunc);
    out << kDefaultSettings;
}

/** Same as RSDK_main() in RSDKv5/main.cpp: link the game logic and run the engine loop. */
int32 RunEngine(int32 argc, char **argv)
{
    RSDK::linkGameLogic = (RSDK::LogicLinkHandle)LinkGameLogicDLL;

    RSDK::InitCoreAPI();
    const int32 exitCode = RSDK::RunRetroEngine(argc, argv);
    RSDK::ReleaseCoreAPI();

    return exitCode;
}

} // namespace

int32 main(int32 argc, char *argv[])
{
    swm::RemoteInput::ApplyPlatformHints();

    // The engine reads and writes everything relative to the working directory.
    swm::Storage storage = swm::Storage::Resolve();
    if (chdir(storage.Root().c_str()) != 0) {
        std::fprintf(stderr, "SonicWebMania: cannot enter %s\n", storage.Root().c_str());
        return 1;
    }

    const int applied = storage.ApplyPendingFiles();
    if (applied > 0)
        std::printf("SonicWebMania: applied %d file(s) uploaded during the last session\n", applied);
    storage.TrimLog();
    WriteDefaultSettingsIfMissing(storage);

    swm::PortConfig config(storage.PathFor(swm::PortConfig::kFileName));
    swm::TransferServer server(storage, config, swm::GeneratePin());
    const bool serverRunning = server.Start();
    if (serverRunning)
        std::printf("SonicWebMania: transfer page on port %u, PIN %s\n", static_cast<unsigned>(server.Port()), server.Pin().c_str());
    else
        std::fprintf(stderr, "SonicWebMania: file transfer server could not bind a port\n");
    std::fflush(stdout);

    if (!storage.HasValidDataPack()) {
        server.SetSetupMode(true);
        const swm::SetupResult result = swm::RunSetupScreen(storage, serverRunning ? &server : nullptr);
        if (result != swm::SetupResult::DataReady) {
            server.Stop();
            return result == swm::SetupResult::Failed ? 1 : 0;
        }
    }

    server.SetSetupMode(false);
    swm::PortRuntime &runtime = swm::PortRuntime::Instance();
    runtime.Configure(serverRunning ? &server : nullptr, swm::FindLanAddress(), config.IsPlusEnabled());

    const int32 exitCode = RunEngine(argc, argv);
    server.Stop();

    if (runtime.RestartRequested()) {
        // A fresh process is the only clean way to reset all of the engine's globals.
        execv("/proc/self/exe", argv);
        std::perror("SonicWebMania: restart failed");
    }

    return exitCode;
}
