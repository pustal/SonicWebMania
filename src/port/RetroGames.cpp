/**
 * @file RetroGames.cpp
 * @brief Finds the retro games' data packs (see RetroGames.h).
 */
#include "RetroGames.h"

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

const std::vector<FanGameInfo> &FanCatalogue()
{
    static const std::vector<FanGameInfo> catalogue = {
        { "nexus", "Sonic Nexus", "sonic-nexus.bin", "Data.bin from Sonic Nexus (2008), any name: it is stored as sonic-nexus.bin" },
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

std::vector<std::string> RetroLibrary::PackFolders() const
{
    std::vector<std::string> folders = { TvPackFolder() };
    for (const UsbDrive &drive : ListUsbDrives())
        folders.push_back(UsbPackFolder(drive.mountPoint));
    return folders;
}

int RetroLibrary::ApplyPendingPacks() const
{
    const std::vector<std::string> folders = PackFolders();
    int applied                            = 0;
    // The fan games' files, replaced while the game read them, as the packs below.
    const std::vector<std::string> fanFolders = FanFolders();
    for (const FanGameInfo &info : FanCatalogue()) {
        for (const std::string &folder : fanFolders) {
            const std::string target  = folder + "/" + info.fileName;
            const std::string pending = Storage::PendingPathFor(target);
            std::error_code ec;
            if (!std::filesystem::exists(pending, ec))
                continue;
            if (InspectFanFile(pending) != PackState::Ready) {
                std::filesystem::remove(pending, ec);
                continue;
            }
            std::filesystem::rename(pending, target, ec);
            if (ec)
                continue;
            ++applied;
            for (const std::string &other : fanFolders) {
                if (other != folder)
                    std::filesystem::remove(other + "/" + info.fileName, ec);
            }
        }
    }
    for (const RetroGameInfo &info : RetroCatalogue()) {
        for (const RetroEdition edition : { RetroEdition::Origins, RetroEdition::Classic }) {
            const std::string fileName = PackFileName(info.id, edition);
            for (const std::string &folder : folders) {
                const std::string target  = folder + "/" + fileName;
                const std::string pending = Storage::PendingPathFor(target);
                std::error_code ec;
                if (!std::filesystem::exists(pending, ec))
                    continue;
                if (Inspect(pending, nullptr) != PackState::Ready) {
                    std::filesystem::remove(pending, ec);
                    continue;
                }
                std::filesystem::rename(pending, target, ec);
                if (ec)
                    continue;
                ++applied;
                // One copy only: the TV's folder is searched first.
                for (const std::string &other : folders) {
                    if (other != folder)
                        std::filesystem::remove(other + "/" + fileName, ec);
                }
            }
        }
    }
    return applied;
}

std::vector<RetroGame> RetroLibrary::Scan() const
{
    // The TV first, then each drive: (folder, is a USB drive).
    std::vector<std::pair<std::string, bool>> folders;
    for (const std::string &folder : PackFolders())
        folders.emplace_back(folder, !folders.empty());

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

const FanGameInfo *RetroLibrary::FindFanGame(const std::string &id)
{
    for (const FanGameInfo &info : FanCatalogue()) {
        if (id == info.id)
            return &info;
    }
    return nullptr;
}

std::string RetroLibrary::TvFanFolder() const { return storage_.PathFor(kFanFolderName); }

std::string RetroLibrary::UsbFanFolder(const std::string &mountPoint)
{
    return mountPoint + "/" + kUsbFolderName + "/" + kFanFolderName;
}

std::vector<std::string> RetroLibrary::FanFolders() const
{
    std::vector<std::string> folders = { TvFanFolder() };
    for (const UsbDrive &drive : ListUsbDrives())
        folders.push_back(UsbFanFolder(drive.mountPoint));
    return folders;
}

PackState RetroLibrary::InspectFanFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return PackState::Missing;
    // u32 header size (little-endian), u8 directory count.
    std::array<unsigned char, 5> header{};
    in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::uint64_t size = Storage::FileSize(path);
    if (in.gcount() < static_cast<std::streamsize>(header.size()) || size < kMinimumPackBytes)
        return PackState::Invalid;
    const std::uint64_t headerSize = static_cast<std::uint64_t>(header[0]) | (static_cast<std::uint64_t>(header[1]) << 8)
                                     | (static_cast<std::uint64_t>(header[2]) << 16) | (static_cast<std::uint64_t>(header[3]) << 24);
    return headerSize > 5 && headerSize < size && header[4] > 0 ? PackState::Ready : PackState::Invalid;
}

std::vector<FanGame> RetroLibrary::ScanFan() const
{
    std::vector<FanGame> games;
    const std::vector<std::string> folders = FanFolders();
    for (const FanGameInfo &info : FanCatalogue()) {
        FanGame game;
        game.info = info;
        for (std::size_t f = 0; f < folders.size(); ++f) {
            const std::string path = folders[f] + "/" + info.fileName;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                continue;
            game.file.path  = path;
            game.file.size  = Storage::FileSize(path);
            game.file.onUsb = f > 0;
            game.file.state = InspectFanFile(path);
            break;
        }
        games.push_back(game);
    }
    return games;
}

bool RetroLibrary::AnyPlayable() const
{
    if (!ScanPlayable().empty())
        return true;
    for (const FanGame &game : ScanFan()) {
        if (game.file.state == PackState::Ready)
            return true;
    }
    return false;
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

} // namespace swm
