/**
 * @file main.cpp
 * @brief Entry point of the webOS build: prepares storage, starts the transfer
 *        server, shows the setup screen when no game data is installed, then
 *        hands over to the engine (RSDKv5-Decompilation-Plus) running Sonic
 *        Mania and, through its Plus module, the retro games.
 *
 * The engine's Plus module does the games' side: which game starts (the last
 * one played, Plus.ini lastGame), switching games from the Plus menu (Back on
 * the remote), the retro games' packs, saves, modes, achievements and Sonic
 * Mania's audio for them, and the Plus DLC opt-in (off by default). This file
 * tells it what only the TV app knows: the packs on USB drives, the transfer
 * page's address, and settings changed on the page (applied once per frame,
 * on the engine's thread). The remote's buttons are mapped by PortRuntime.
 * Mods (the engine's mod loader) are managed on the page; before the engine
 * starts, staged mod changes are applied and the USB drives' mods linked in
 * (see port/ModLibrary.h). Sonic Origins' sound and video files are uploaded
 * on the page too, and converted by the engine when the page asks (its Plus
 * module, RSDK::Plus::StartOriginsAudioConversion() and
 * StartOriginsVideoConversion()).
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

#include "port/IniEdit.h"
#include "port/ModLibrary.h"
#include "port/NetInfo.h"
#include "port/PlusFiles.h"
#include "port/PortRuntime.h"
#include "port/RemoteInput.h"
#include "port/RetroGames.h"
#include "port/SettingsChannel.h"
#include "port/SetupScreen.h"
#include "port/Storage.h"
#include "port/TransferServer.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#if !RETRO_USE_PLUS
#error "SonicWebMania needs the engine's Plus module: RETRO_PLUS, RETRO_MOD_LOADER and RETRO_REVISION 3 (see CMakeLists.txt)"
#endif

namespace
{

/**
 * Engine settings suited to a TV, written only on first launch (the engine
 * rewrites Settings.ini itself whenever the player changes options).
 */
constexpr const char *kDefaultSettings = "[Game]\n"
                                         "devMenu=n\n"
                                         "faceButtonFlip=n\n"
                                         "plusDLC=n\n"
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

/** The engine's settings as the page sees them, and back. */
swm::GameSettings ToGameSettings(const RSDK::Plus::Settings &settings)
{
    swm::GameSettings converted;
    converted.plusDLC      = settings.plusDLC;
    converted.maniaAudio   = settings.maniaAudio;
    converted.similarMusic = settings.similarMusic;
    converted.maniaRemixes = settings.maniaRemixes;
    return converted;
}

RSDK::Plus::Settings ToPlusSettings(const swm::GameSettings &settings)
{
    RSDK::Plus::Settings converted;
    converted.plusDLC      = settings.plusDLC;
    converted.maniaAudio   = settings.maniaAudio;
    converted.similarMusic = settings.similarMusic;
    converted.maniaRemixes = settings.maniaRemixes;
    return converted;
}

/** A Sonic Origins conversion's state as the engine gives it, for the page. */
swm::OriginsState ToOriginsState(const RSDK::Plus::OriginsAudioStatus &status)
{
    swm::OriginsState state;
    state.running = status.running;
    state.percent = status.percent;
    state.message = status.message;
    return state;
}

/** What the engine's callbacks (plain function pointers) reach. */
struct Session {
    swm::SettingsChannel *settings = nullptr;
    swm::TransferServer *server    = nullptr;
    const swm::RetroLibrary *retro = nullptr;
    std::string maniaPack;                  ///< Sonic Mania's pack this session ("" without it).
    bool restartRequested  = false;         ///< "Restart game" was chosen in the Plus menu.
    bool originsConverting = false;         ///< A Sonic Origins conversion (audio or videos) was running last frame.
};
Session session;

/**
 * Every pack the engine may read this session (see TransferServer::SetPacksInUse()):
 * Sonic Mania's, the retro games' and their converted Sonic Origins audio and videos.
 */
std::vector<std::string> PacksInUse(const std::string &maniaPack, const swm::RetroLibrary &retro)
{
    std::vector<std::string> packs;
    if (!maniaPack.empty())
        packs.push_back(maniaPack);
    for (const swm::RetroGame &game : retro.Scan()) {
        for (const swm::RetroPack *pack : { &game.origins, &game.classic }) {
            if (pack->state == swm::PackState::Ready || pack->state == swm::PackState::Unsupported)
                packs.push_back(pack->path);
        }
    }
    for (const std::string &folder : retro.PackFolders()) {
        for (const swm::RetroGameInfo &game : swm::RetroCatalogue()) {
            for (const char *suffix : { swm::TransferServer::kOriginsPackSuffix, swm::TransferServer::kOriginsVideoPackSuffix }) {
                const std::string path = folder + "/" + game.id + suffix;
                if (swm::Storage::FileSize(path) > 0)
                    packs.push_back(path);
            }
        }
    }
    return packs;
}

/**
 * Once per frame on the engine's thread (RSDK::Plus::SetFrameCallback()):
 * the remote's mapping, then a settings change asked for on the web page,
 * then the settings as they are now, for the page to show.
 */
void OnEngineFrame()
{
    swm::PortRuntime::Instance().OnFrame(RSDK::Plus::MenuOpen());

    swm::GameSettings requested;
    if (session.settings->TakeRequest(&requested))
        RSDK::Plus::ApplySettings(ToPlusSettings(requested));
    session.settings->Publish(ToGameSettings(RSDK::Plus::CurrentSettings()));

    // A conversion wrote new packs, which the engine now reads: the page must leave them alone.
    const bool converting = RSDK::Plus::GetOriginsAudioStatus().running || RSDK::Plus::GetOriginsVideoStatus().running;
    if (session.originsConverting && !converting && session.server)
        session.server->SetPacksInUse(PacksInUse(session.maniaPack, *session.retro));
    session.originsConverting = converting;
}

/**
 * The Plus menu's "Restart game": closes the engine, and main() starts the
 * app again, which applies the files the page staged (a new Data.rsdk or
 * pack, saves) and finds packs added since.
 */
void RequestRestart()
{
    session.restartRequested     = true;
    RSDK::RenderDevice::isRunning = false;
}


/** Tells the Plus module what only the app knows: the USB drives' packs, the page's address, "Restart game". */
void ConfigurePlus(const swm::RetroLibrary &retro, const swm::TransferServer *server)
{
    const std::vector<std::string> folders = retro.PackFolders();
    for (std::size_t i = 1; i < folders.size(); ++i) // [0] is the TV's, which the engine finds itself.
        RSDK::Plus::AddPackFolder(folders[i]);

    RSDK::Plus::PlatformTexts texts;
    const std::string address = swm::FindLanAddress();
    if (server && !address.empty()) {
        const std::string page = "http://" + address + ":" + std::to_string(server->Port());
        texts.menuInfo         = "Transfer page: " + page + "  PIN " + server->Pin();
        texts.packHint         = "Add retro packs from the transfer page on a phone or computer: " + page
                         + ". Then choose Restart game.";
        texts.originsAudioHint = "Upload them on the transfer page (" + page + "), then press OK here or Convert there.";
        texts.originsVideoHint = texts.originsAudioHint;
    }
    else {
        texts.menuInfo         = "Connect the TV to a network to reach the transfer page.";
        texts.packHint         = "Add retro packs from the transfer page (the TV must be on a network), then choose Restart game.";
        texts.originsAudioHint = "Upload them on the transfer page (the TV must be on a network), then press OK here.";
        texts.originsVideoHint = texts.originsAudioHint;
    }
    RSDK::Plus::SetPlatformTexts(texts);
    RSDK::Plus::AddMenuAction("Restart game", &RequestRestart);
    RSDK::Plus::SetFrameCallback(&OnEngineFrame);
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

    swm::RetroLibrary retro(storage);
    swm::ModLibrary mods(storage);
    const int applied = storage.ApplyPendingFiles() + retro.ApplyPendingPacks() + mods.ApplyPending();
    if (applied > 0)
        std::printf("SonicWebMania: applied %d file(s) uploaded or moved during the last session\n", applied);
    storage.TrimLog();
    WriteDefaultSettingsIfMissing(storage);
    swm::MigrateEarlierVersion(storage, kDefaultSettings);

    swm::SettingsChannel settings(swm::ReadSavedSettings(storage));
    session.settings = &settings;
    swm::TransferServer server(storage, settings, retro, mods, swm::GeneratePin());
    swm::OriginsHooks audio, video;
    audio.fileNames.assign(std::begin(RSDK::Plus::kOriginsAudioFileNames), std::end(RSDK::Plus::kOriginsAudioFileNames));
    audio.packSuffix = swm::TransferServer::kOriginsPackSuffix;
    audio.start      = [] { return RSDK::Plus::StartOriginsAudioConversion() != 0; };
    audio.status     = [] { return ToOriginsState(RSDK::Plus::GetOriginsAudioStatus()); };
    video.fileNames.assign(std::begin(RSDK::Plus::kOriginsVideoFileNames), std::end(RSDK::Plus::kOriginsVideoFileNames));
    video.packSuffix  = swm::TransferServer::kOriginsVideoPackSuffix;
    video.convertible = RSDK::Plus::OriginsVideoConversionAvailable() != 0;
    video.start       = [] { return RSDK::Plus::StartOriginsVideoConversion() != 0; };
    video.status      = [] { return ToOriginsState(RSDK::Plus::GetOriginsVideoStatus()); };
    server.SetOriginsHooks(std::move(audio), std::move(video));
    session.server = &server;
    session.retro  = &retro;
    const bool serverRunning = server.Start();
    if (serverRunning)
        std::printf("SonicWebMania: transfer page on port %u, PIN %s\n", static_cast<unsigned>(server.Port()), server.Pin().c_str());
    else
        std::fprintf(stderr, "SonicWebMania: file transfer server could not bind a port\n");
    std::fflush(stdout);

    // Sonic Mania's pack, on the TV or a USB drive ("" when not installed). It is optional:
    // the app can be used for the retro games alone.
    std::string maniaPack = storage.DataPackPath();
    const std::function<bool()> retroReady = [&retro] { return !retro.ScanPlayable().empty(); };
    if (maniaPack.empty() && !retroReady()) {
        server.SetSetupMode(true);
        const swm::SetupResult result = swm::RunSetupScreen(storage, serverRunning ? &server : nullptr, retroReady);
        if (result != swm::SetupResult::DataReady) {
            server.Stop();
            return result == swm::SetupResult::Failed ? 1 : 0;
        }
        maniaPack = storage.DataPackPath();
    }

    // The pack may have moved between the TV and a USB drive since the last start. Without it,
    // the engine starts the first retro game instead (its Plus module).
    std::string error;
    const std::string dataFile = maniaPack.empty() ? std::string("Data.rsdk") : maniaPack;
    if (!swm::UpdateIniFile(storage.PathFor("Settings.ini"), kDefaultSettings, "Game", { { "dataFile", dataFile } }, &error))
        std::fprintf(stderr, "SonicWebMania: %s\n", error.c_str());

    session.maniaPack = maniaPack;
    server.SetPacksInUse(PacksInUse(maniaPack, retro));
    // The engine finds the drives' mods through links in its own mod folder; those on now are read all session.
    mods.LinkUsbMods();
    server.SetModsInUse(mods.ActiveFolders());
    server.SetSetupMode(false);
    ConfigurePlus(retro, serverRunning ? &server : nullptr);
    std::fflush(stdout);

    const int32 exitCode = RunEngine(argc, argv);
    server.Stop();

    if (session.restartRequested) {
        // A fresh process is the only clean way to reset all of the engine's globals.
        execv("/proc/self/exe", argv);
        std::perror("SonicWebMania: restart failed");
    }

    return exitCode;
}
