/**
 * @file main.cpp
 * @brief Entry point of the webOS build: prepares storage, starts the transfer
 *        server, shows the setup screen when no game data is installed, then
 *        hands over to the RSDKv5 engine running Sonic Mania or, when retro
 *        mode is on, one of the retro games (see port/RetroGames.h).
 *
 * Which game runs:
 *   1. the one named by --swm-game=<id> (how the Back menu's "Retro games"
 *      page and "Restart game" relaunch the app);
 *   2. otherwise the last game left normally (port.ini lastGame);
 *   3. otherwise Sonic Mania, or the first playable retro game when there is
 *      no Sonic Mania data.
 * A game id that cannot be played (its pack was removed, retro mode was
 * switched off) falls through to the next rule.
 *
 * Replaces RSDKv5/main.cpp from the engine (excluded in cmake/RSDKPlatform-webOS.cmake).
 */
#include "RSDK/Core/RetroEngine.hpp"
#include "RetroSaves.h"

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
#include "port/RetroGames.h"
#include "port/SetupScreen.h"
#include "port/Storage.h"
#include "port/TransferServer.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace
{

/** Command-line option naming the game to start; never passed on to the engine. */
constexpr const char *kGameOption = "--swm-game=";

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

/**
 * Copies the engine's achievement list for the Back menu. The engine registers
 * Sonic Mania's achievements at start-up and updates them as the game unlocks
 * them (saved in Achievements.bin), so this always reflects the current state.
 */
std::vector<swm::AchievementEntry> ReadAchievements()
{
    std::vector<swm::AchievementEntry> entries;
    entries.reserve(RSDK::achievementList.size());
    for (const RSDK::AchievementInfo &info : RSDK::achievementList)
        entries.push_back(swm::AchievementEntry{ info.name, info.description, info.achieved != 0 });
    return entries;
}

/** The retro games the player can start now: none unless retro mode is on. */
std::vector<swm::RetroGame> PlayableRetroGames(const swm::PortConfig &config, const swm::RetroLibrary &retro)
{
    return config.IsRetroEnabled() ? retro.ScanPlayable() : std::vector<swm::RetroGame>{};
}

/** True when @p id is Sonic Mania with its data installed, or one of @p retroGames. */
bool CanPlay(const std::string &id, bool maniaReady, const std::vector<swm::RetroGame> &retroGames)
{
    if (id == swm::RetroLibrary::kManiaId)
        return maniaReady;
    for (const swm::RetroGame &game : retroGames) {
        if (id == game.info.id)
            return true;
    }
    return false;
}

/** Picks the game to start (see the file comment). @return "" when nothing can be played. */
std::string ChooseGame(const std::string &requested, const std::string &lastGame, bool maniaReady,
                       const std::vector<swm::RetroGame> &retroGames)
{
    for (const std::string &candidate : { requested, lastGame }) {
        if (!candidate.empty() && CanPlay(candidate, maniaReady, retroGames))
            return candidate;
    }
    if (maniaReady)
        return swm::RetroLibrary::kManiaId;
    return retroGames.empty() ? std::string() : std::string(retroGames.front().info.id);
}

/** The Back menu's game list: Sonic Mania (when installed) and the playable retro games. */
std::vector<swm::GameChoice> BuildGameChoices(bool maniaReady, const std::vector<swm::RetroGame> &retroGames)
{
    std::vector<swm::GameChoice> choices;
    if (maniaReady)
        choices.push_back(swm::GameChoice{ swm::RetroLibrary::kManiaId, "Sonic Mania", "" });
    for (const swm::RetroGame &game : retroGames)
        choices.push_back(swm::GameChoice{ game.info.id, game.info.title, game.ModeDescription() });
    return choices;
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

    // Our own option is taken out; everything else goes to the engine.
    std::string requestedGame;
    std::vector<char *> engineArgv;
    for (int32 i = 0; i < argc; ++i) {
        if (i > 0 && std::strncmp(argv[i], kGameOption, std::strlen(kGameOption)) == 0)
            requestedGame = argv[i] + std::strlen(kGameOption);
        else
            engineArgv.push_back(argv[i]);
    }
    const int32 engineArgc = static_cast<int32>(engineArgv.size());
    engineArgv.push_back(nullptr);

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
    swm::RetroLibrary retro(storage);
    swm::TransferServer server(storage, config, retro, swm::GeneratePin());
    const bool serverRunning = server.Start();
    if (serverRunning)
        std::printf("SonicWebMania: transfer page on port %u, PIN %s\n", static_cast<unsigned>(server.Port()), server.Pin().c_str());
    else
        std::fprintf(stderr, "SonicWebMania: file transfer server could not bind a port\n");
    std::fflush(stdout);

    bool maniaReady                        = storage.HasValidDataPack();
    std::vector<swm::RetroGame> retroGames = PlayableRetroGames(config, retro);
    if (!maniaReady && retroGames.empty()) {
        server.SetSetupMode(true);
        const std::function<bool()> retroReady = [&config, &retro] { return !PlayableRetroGames(config, retro).empty(); };
        const swm::SetupResult result = swm::RunSetupScreen(storage, serverRunning ? &server : nullptr, retroReady);
        if (result != swm::SetupResult::DataReady) {
            server.Stop();
            return result == swm::SetupResult::Failed ? 1 : 0;
        }
        maniaReady = storage.HasValidDataPack();
        retroGames = PlayableRetroGames(config, retro);
    }

    std::string game = ChooseGame(requestedGame, config.LastGame(), maniaReady, retroGames);
    if (game != swm::RetroLibrary::kManiaId) {
        // A retro game runs in its own folder, whose Settings.ini points at its packs.
        const swm::RetroGame *chosen = nullptr;
        for (const swm::RetroGame &candidate : retroGames) {
            if (game == candidate.info.id)
                chosen = &candidate;
        }
        std::string error = "no game to play";
        if (chosen && retro.PrepareLaunch(*chosen, kDefaultSettings, &error) && chdir(retro.WorkFolder(game).c_str()) == 0) {
            std::printf("SonicWebMania: starting %s (%s)\n", chosen->info.title, chosen->ModeDescription().c_str());
        }
        else {
            std::fprintf(stderr, "SonicWebMania: cannot start %s: %s\n", game.c_str(), error.c_str());
            if (!maniaReady || chdir(storage.Root().c_str()) != 0) {
                server.Stop();
                return 1;
            }
            game = swm::RetroLibrary::kManiaId;
        }
    }
    std::fflush(stdout);

    // Forget the game while it runs: if it crashes, the next launch does not start it again.
    config.SetLastGame("");

    server.SetSetupMode(false);
    swm::PortRuntime &runtime = swm::PortRuntime::Instance();
    runtime.SetGames(BuildGameChoices(maniaReady, retroGames), game);
    runtime.Configure(serverRunning ? &server : nullptr, swm::FindLanAddress(), config.IsPlusEnabled(), &ReadAchievements);

    // Sonic Origins' coins, shared by the retro games (listed with the saves on the web page).
    swm::ConfigureRetroSession(storage.PathFor("RetroCoins.ini"), game);

    const int32 exitCode = RunEngine(engineArgc, engineArgv.data());
    swm::FinishRetroSession();
    server.Stop();

    // Left normally: remember the game (or the one chosen in the menu) for the next launch.
    const std::string nextGame = runtime.RestartRequested() ? runtime.NextGame() : game;
    config.SetLastGame(nextGame);

    if (runtime.RestartRequested()) {
        // A fresh process is the only clean way to reset all of the engine's globals.
        std::string gameOption          = std::string(kGameOption) + nextGame;
        std::vector<char *> restartArgv = { argv[0], &gameOption[0] };
        restartArgv.insert(restartArgv.end(), engineArgv.begin() + 1, engineArgv.end()); // Includes the final null.
        execv("/proc/self/exe", restartArgv.data());
        std::perror("SonicWebMania: restart failed");
    }

    return exitCode;
}
