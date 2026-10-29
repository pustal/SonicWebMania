/**
 * @file RetroGames.cpp
 * @brief Finds and prepares the retro games' data packs (see RetroGames.h).
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

/** Settings.ini values Game:gameType understands (RSDKv5U's legacy cores). */
constexpr int kGameTypeStandalone = 0; ///< The earlier releases.
constexpr int kGameTypeOrigins    = 1; ///< Sonic Origins.

/** Removes leading and trailing blanks. */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/** Reads a whole text file ("" when it does not exist). */
std::string ReadText(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/**
 * Returns @p ini with the keys of @p values set inside section [@p section]:
 * existing lines are replaced in place, missing keys are added at the end of
 * the section, and the section is appended when the file has none. Keys are
 * compared without regard to case, as the engine's ini parser does.
 */
std::string SetIniValues(const std::string &ini, const std::string &section,
                         const std::vector<std::pair<std::string, std::string>> &values)
{
    auto lower = [](std::string text) {
        for (char &c : text)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return text;
    };

    std::vector<std::string> lines;
    {
        std::istringstream in(ini);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    std::vector<bool> written(values.size(), false);
    const std::string wantedSection = lower(section);
    bool inSection                  = false;
    std::size_t sectionEnd          = std::string::npos; // Index after the last line of the section.

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(lines[i]);
        if (!trimmed.empty() && trimmed.front() == '[') {
            const std::size_t close = trimmed.find(']');
            inSection               = close != std::string::npos && lower(Trim(trimmed.substr(1, close - 1))) == wantedSection;
            if (inSection)
                sectionEnd = i + 1;
            continue;
        }
        if (!inSection)
            continue;
        if (!trimmed.empty())
            sectionEnd = i + 1;

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = lower(Trim(trimmed.substr(0, equals)));
        for (std::size_t v = 0; v < values.size(); ++v) {
            if (key == lower(values[v].first)) {
                lines[i]   = values[v].first + "=" + values[v].second;
                written[v] = true;
            }
        }
    }

    std::vector<std::string> missing;
    for (std::size_t v = 0; v < values.size(); ++v) {
        if (!written[v])
            missing.push_back(values[v].first + "=" + values[v].second);
    }
    if (sectionEnd == std::string::npos) {
        if (!lines.empty() && !Trim(lines.back()).empty())
            lines.emplace_back();
        lines.push_back("[" + section + "]");
        lines.insert(lines.end(), missing.begin(), missing.end());
    }
    else {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), missing.begin(), missing.end());
    }

    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

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
        return "Sonic Origins, no sound (add the classic pack for sound)";
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

bool RetroLibrary::PrepareLaunch(const RetroGame &game, const std::string &defaultSettings, std::string *error) const
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

    const std::string settingsPath = folder + "/Settings.ini";
    std::string ini                = ReadText(settingsPath);
    if (Trim(ini).empty())
        ini = defaultSettings;
    ini = SetIniValues(ini, "Game",
                       { { "dataFile", dataFile }, { "extraDataFile", extraFile }, { "gameType", std::to_string(gameType) } });

    const std::string partPath = settingsPath + ".part";
    {
        std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
        out << ini;
        if (!out) {
            if (error)
                *error = "cannot write " + settingsPath;
            return false;
        }
    }
    std::filesystem::rename(partPath, settingsPath, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + settingsPath + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace swm
