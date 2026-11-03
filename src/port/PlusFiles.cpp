/**
 * @file PlusFiles.cpp
 * @brief See PlusFiles.h.
 */
#include "PlusFiles.h"

#include "IniEdit.h"
#include "RetroGames.h"
#include "Storage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

namespace swm
{
namespace
{

namespace fs = std::filesystem;

/** Files of an earlier SonicWebMania (see PlusFiles.h). */
constexpr const char *kOldConfigName = "port.ini";
constexpr const char *kOldCoinsName  = "RetroCoins.ini";
constexpr const char *kOldGamesName  = "games";

/** The engine's settings file and Plus.ini's last line (Plus.cpp stops reading there). */
constexpr const char *kSettingsIniName = "Settings.ini";
constexpr const char *kPlusIniEndLine  = "; end";

/** Removes leading and trailing blanks. */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/** The usual spellings of an ini boolean, as the engine reads them. */
bool ParseBool(const std::string &value)
{
    const std::string lower = Lower(value);
    return lower == "1" || lower == "y" || lower == "yes" || lower == "true";
}

/**
 * The "key=value" lines of @p path, keys in lower case: those of section
 * [@p section] ("" for a file without sections), up to a line equal to
 * @p endLine ("" for none). Comments (';', '#') and other sections are skipped.
 */
std::map<std::string, std::string> ReadKeys(const std::string &path, const std::string &section, const std::string &endLine)
{
    std::map<std::string, std::string> keys;
    std::ifstream in(path);
    std::string line;
    std::string current;
    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);
        if (!endLine.empty() && trimmed == endLine)
            break;
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
            continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            current = Lower(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos || current != Lower(section))
            continue;
        keys[Lower(Trim(trimmed.substr(0, equals)))] = Trim(trimmed.substr(equals + 1));
    }
    return keys;
}

/** @p keys[@p key] as a boolean, or @p fallback when it is missing. */
bool BoolOr(const std::map<std::string, std::string> &keys, const std::string &key, bool fallback)
{
    const auto found = keys.find(Lower(key));
    return found == keys.end() ? fallback : ParseBool(found->second);
}

/** Plus.ini with these values, in the engine's own layout (RSDK/Plus/Plus.cpp SaveConfig()). */
std::string PlusIniText(const GameSettings &settings, const std::string &lastGame, int coins)
{
    std::string text = "; Plus settings (see RSDK/Plus/Plus.cpp)\n";
    if (!lastGame.empty())
        text += "lastGame=" + lastGame + "\n";
    text += std::string("maniaAudio=") + (settings.maniaAudio ? "1" : "0") + "\n";
    text += std::string("maniaSimilarMusic=") + (settings.similarMusic ? "1" : "0") + "\n";
    text += std::string("maniaRemixes=") + (settings.maniaRemixes ? "1" : "0") + "\n";
    text += "coins=" + std::to_string(coins) + "\n";
    text += std::string(kPlusIniEndLine) + "\n";
    return text;
}

/** Moves the files of an earlier version's games/<id>/ into the engine's retro/<id>/ (see MigrateEarlierVersion()). */
int MoveRetroSaves(const Storage &storage)
{
    int moved = 0;
    std::error_code ec;
    for (const RetroGameInfo &game : RetroCatalogue()) {
        const fs::path from = storage.PathFor(std::string(kOldGamesName) + "/" + game.id);
        const fs::path to   = storage.PathFor(std::string(Storage::kSavesFolderName) + "/" + game.id);
        for (const fs::directory_entry &entry : fs::directory_iterator(from, ec)) {
            const std::string name = entry.path().filename().string();
            std::error_code fileError;
            // Saves only: the game's own Settings.ini (now the one Settings.ini) and log stay behind.
            if (!entry.is_regular_file(fileError) || name.size() < 4 || name.compare(name.size() - 4, 4, ".bin") != 0)
                continue;
            fs::create_directories(to, fileError);
            if (fs::exists(to / name, fileError))
                continue; // A save the engine wrote already wins.
            fs::rename(entry.path(), to / name, fileError);
            moved += fileError ? 0 : 1;
        }
    }
    return moved;
}

/**
 * Removes what is left of an earlier version's games/ folder: each game's
 * Settings.ini and log, then the folders once empty. A save that could not
 * be moved stays, with its folder.
 */
void RemoveOldGameFolders(const Storage &storage)
{
    std::error_code ec;
    const fs::path games = storage.PathFor(kOldGamesName);
    for (const RetroGameInfo &game : RetroCatalogue()) {
        const fs::path folder = games / game.id;
        fs::remove(folder / kSettingsIniName, ec);
        fs::remove(folder / "log.txt", ec);
        fs::remove(folder, ec); // Only when empty.
    }
    fs::remove(games, ec); // Only when empty.
}

} // namespace

GameSettings ReadSavedSettings(const Storage &storage)
{
    const auto plus     = ReadKeys(storage.PathFor(kPlusIniName), "", kPlusIniEndLine);
    const auto settings = ReadKeys(storage.PathFor(kSettingsIniName), "Game", "");

    GameSettings saved;
    saved.plusDLC      = BoolOr(settings, "plusDLC", saved.plusDLC);
    saved.maniaAudio   = BoolOr(plus, "maniaAudio", saved.maniaAudio);
    saved.similarMusic = BoolOr(plus, "maniaSimilarMusic", saved.similarMusic);
    saved.maniaRemixes = BoolOr(plus, "maniaRemixes", saved.maniaRemixes);
    return saved;
}

bool MigrateEarlierVersion(const Storage &storage, const char *defaultSettings)
{
    const std::string oldConfig = storage.PathFor(kOldConfigName);
    std::error_code ec;
    if (!fs::exists(oldConfig, ec))
        return false;

    const auto old = ReadKeys(oldConfig, "", "");
    GameSettings settings;
    settings.plusDLC      = BoolOr(old, "plusEnabled", false);
    settings.maniaAudio   = BoolOr(old, "maniaAudio", settings.maniaAudio);
    settings.similarMusic = BoolOr(old, "maniaSimilarMusic", settings.similarMusic);
    settings.maniaRemixes = BoolOr(old, "maniaRemixes", settings.maniaRemixes);
    const auto lastGame   = old.find("lastgame");

    int coins = 0;
    const auto coinKeys = ReadKeys(storage.PathFor(kOldCoinsName), "", "");
    if (coinKeys.count("coins")) {
        try {
            coins = std::clamp(std::stoi(coinKeys.at("coins")), 0, 999);
        }
        catch (const std::exception &) {
            coins = 0;
        }
    }

    // Plus.ini: only when the engine has not written its own yet.
    const std::string plusIni = storage.PathFor(kPlusIniName);
    if (!fs::exists(plusIni, ec)) {
        std::ofstream out(plusIni, std::ios::trunc);
        out << PlusIniText(settings, lastGame == old.end() ? std::string() : lastGame->second, coins);
        if (!out)
            std::fprintf(stderr, "SonicWebMania: could not write %s\n", plusIni.c_str());
    }

    std::string error;
    if (!UpdateIniFile(storage.PathFor(kSettingsIniName), defaultSettings, "Game", { { "plusDLC", settings.plusDLC ? "y" : "n" } },
                       &error))
        std::fprintf(stderr, "SonicWebMania: %s\n", error.c_str());

    const int moved = MoveRetroSaves(storage);
    std::printf("SonicWebMania: moved the settings of an earlier version, and %d retro save file(s), to the engine's files\n",
                moved);

    fs::remove(oldConfig, ec);
    fs::remove(storage.PathFor(kOldCoinsName), ec);
    RemoveOldGameFolders(storage);
    return true;
}

} // namespace swm
