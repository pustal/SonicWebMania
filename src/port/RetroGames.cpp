/**
 * @file RetroGames.cpp
 * @brief Finds and prepares the retro games' data packs (see RetroGames.h).
 */
#include "RetroGames.h"

#include "IniEdit.h"
#include "RsdkPack.h"
#include "Storage.h"
#include "UsbDrives.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace swm
{
namespace
{

/** Smallest file that can be a pack: the 6-byte header plus a file table. */
constexpr std::uint64_t kMinimumPackBytes = 64;

/** Settings.ini values Game:gameType understands (RSDKv5U's legacy cores). */
constexpr int kGameTypeStandalone = 0; ///< The earlier releases.
constexpr int kGameTypeOrigins    = 1; ///< Sonic Origins.

} // namespace

const std::vector<RetroGameInfo> &RetroCatalogue()
{
    static const std::vector<RetroGameInfo> catalogue = {
        { "sonic1", "Sonic the Hedgehog" },
        { "sonic2", "Sonic the Hedgehog 2" },
        { "soniccd", "Sonic CD" },
    };
    return catalogue;
}

std::string RetroGame::ModeDescription() const
{
    const bool hasOrigins = origins.state == PackState::Ready;
    const bool hasClassic = classic.state == PackState::Ready;
    if (hasOrigins && hasClassic)
        return "Sonic Origins, sound from the classic release";
    if (hasOrigins)
        return "Sonic Origins, no sound pack (sound from the classic pack or Sonic Mania)";
    if (hasClassic && classic.standalone)
        return "Classic release";
    if (hasClassic)
        return "Needs the Origins pack (this release only adds its sound)";
    return "Not installed";
}

const char *PackStateName(PackState state)
{
    switch (state) {
        case PackState::Missing: return "missing";
        case PackState::Ready: return "ready";
        case PackState::Unsupported: return "unsupported";
        case PackState::Invalid: return "invalid";
    }
    return "missing";
}

RetroLibrary::RetroLibrary(const Storage &storage) : storage_(storage) {}

const RetroGameInfo *RetroLibrary::FindGame(const std::string &id)
{
    for (const RetroGameInfo &info : RetroCatalogue()) {
        if (id == info.id)
            return &info;
    }
    return nullptr;
}

std::string RetroLibrary::PackFileName(const std::string &id, RetroEdition edition)
{
    return id + (edition == RetroEdition::Origins ? "-origins.rsdk" : "-classic.rsdk");
}

std::string RetroLibrary::TvPackFolder() const { return storage_.PathFor(kPackFolderName); }

std::string RetroLibrary::UsbPackFolder(const std::string &mountPoint)
{
    return mountPoint + "/" + kUsbFolderName + "/" + kPackFolderName;
}

std::string RetroLibrary::WorkFolder(const std::string &id) const
{
    return storage_.PathFor(std::string(Storage::kGamesFolderName) + "/" + id);
}

PackState RetroLibrary::Inspect(const std::string &path, char *version)
{
    if (version)
        *version = 0;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return PackState::Missing;

    std::array<char, 6> header{};
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (in.gcount() < static_cast<std::streamsize>(header.size()) || Storage::FileSize(path) < kMinimumPackBytes)
        return PackState::Invalid;

    const bool signature = header[0] == 'R' && header[1] == 'S' && header[2] == 'D' && header[3] == 'K';
    if (!signature)
        return RsdkPack::LooksLikeOldV3Pack(path) ? PackState::Unsupported : PackState::Invalid;
    if (header[4] != 'v')
        return PackState::Invalid;

    switch (header[5]) {
        case '3':
        case '4':
        case 'B':
            if (version)
                *version = header[5];
            return PackState::Ready;
        default: return PackState::Invalid; // '5' is Sonic Mania's format.
    }
}

void RetroLibrary::FindPack(const std::vector<std::pair<std::string, bool>> &folders, const std::string &fileName, RetroPack *pack)
{
    for (const auto &folder : folders) {
        const std::string path = folder.first + "/" + fileName;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
            continue;

        pack->path  = path;
        pack->size  = Storage::FileSize(path);
        pack->onUsb = folder.second;
        pack->state      = Inspect(path, &pack->version);
        pack->standalone = pack->state == PackState::Ready && RsdkPack::HasEngineBytecode(path);
        return;
    }
}

std::vector<RetroGame> RetroLibrary::Scan() const
{
    // The TV first, then each drive: (folder, is a USB drive).
    std::vector<std::pair<std::string, bool>> folders = { { TvPackFolder(), false } };
    for (const UsbDrive &drive : ListUsbDrives())
        folders.emplace_back(UsbPackFolder(drive.mountPoint), true);

    std::vector<RetroGame> games;
    for (const RetroGameInfo &info : RetroCatalogue()) {
        RetroGame game;
        game.info = info;
        FindPack(folders, PackFileName(info.id, RetroEdition::Origins), &game.origins);
        FindPack(folders, PackFileName(info.id, RetroEdition::Classic), &game.classic);
        games.push_back(game);
    }
    return games;
}

std::vector<RetroGame> RetroLibrary::ScanPlayable() const
{
    std::vector<RetroGame> playable;
    for (const RetroGame &game : Scan()) {
        if (game.Playable())
            playable.push_back(game);
    }
    return playable;
}

bool RetroLibrary::PrepareLaunch(const RetroGame &game, const std::string &defaultSettings, const std::string &audioPack,
                                 std::string *error) const
{
    if (!game.Playable()) {
        if (error)
            *error = std::string(game.info.title) + " has no playable data pack";
        return false;
    }

    const std::string folder = WorkFolder(game.info.id);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        if (error)
            *error = "cannot create " + folder + ": " + ec.message();
        return false;
    }

    // Origins first: the engine takes each file from the first pack that has
    // it and detects the engine version from the first pack.
    const bool origins          = game.origins.state == PackState::Ready;
    const std::string dataFile  = origins ? game.origins.path : game.classic.path;
    const std::string extraFile = origins && game.HasSound() ? game.classic.path : std::string();
    const int gameType          = origins ? kGameTypeOrigins : kGameTypeStandalone;

    return UpdateIniFile(folder + "/Settings.ini", defaultSettings, "Game",
                         { { "dataFile", dataFile },
                           { "extraDataFile", extraFile },
                           { "audioDataFile", audioPack },
                           { "gameType", std::to_string(gameType) } },
                         error);
}

} // namespace swm
