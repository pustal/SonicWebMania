/**
 * @file ModLibrary.cpp
 * @brief See ModLibrary.h.
 */
#include "ModLibrary.h"

#include "Storage.h"
#include "UsbDrives.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace swm
{
namespace
{

namespace fs = std::filesystem;

constexpr std::size_t kMaxFolderBytes = 64;
constexpr std::size_t kMaxPathBytes   = 240;
constexpr std::size_t kMaxPathDepth   = 16;

/** Removes leading and trailing blanks. */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool EndsWith(const std::string &text, const std::string &suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/** True for a name part a FAT or exFAT drive can store: printable, none of its reserved characters, no trailing dot or space. */
bool IsPortableName(const std::string &part)
{
    if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.')
        return false;
    return std::none_of(part.begin(), part.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F || std::string("<>:\"/\\|?*").find(c) != std::string::npos;
    });
}

/** The "key=value" lines of a mod.ini (no sections, as the engine reads it), values unquoted. */
std::map<std::string, std::string> ReadIni(const std::string &path)
{
    std::map<std::string, std::string> keys;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[')
            continue;
        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;
        std::string value = Trim(trimmed.substr(equals + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        keys[Trim(trimmed.substr(0, equals))] = value;
    }
    return keys;
}

/** @p keys[@p key], or @p fallback. */
std::string ValueOr(const std::map<std::string, std::string> &keys, const char *key, const std::string &fallback)
{
    const auto found = keys.find(key);
    return found == keys.end() || found->second.empty() ? fallback : found->second;
}

/** The usual spellings of an ini boolean, as the engine's iniparser reads them (first letter). */
bool ParseBool(const std::string &value)
{
    return !value.empty() && (value[0] == 'y' || value[0] == 'Y' || value[0] == '1' || value[0] == 't' || value[0] == 'T');
}

/** Removes @p path whether it is a link, a file or a folder (a link's target is left alone). */
void RemovePath(const fs::path &path)
{
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(path, ec)))
        fs::remove(path, ec);
    else
        fs::remove_all(path, ec);
}

} // namespace

ModLibrary::ModLibrary(const Storage &storage) : storage_(storage) {}

std::string ModLibrary::TvFolder() const { return storage_.PathFor(kModsFolderName); }

std::string ModLibrary::UsbFolder(const std::string &mountPoint) { return Storage::UsbFolder(mountPoint) + "/" + kModsFolderName; }

std::vector<std::string> ModLibrary::Folders() const
{
    std::vector<std::string> folders = { TvFolder() };
    for (const UsbDrive &drive : ListUsbDrives())
        folders.push_back(UsbFolder(drive.mountPoint));
    return folders;
}

bool ModLibrary::ReadMod(const std::string &path, const std::string &folder, ModEntry *entry)
{
    std::error_code ec;
    const std::string ini = path + "/" + kModIniName;
    if (!fs::is_regular_file(ini, ec))
        return false;

    const auto keys      = ReadIni(ini);
    entry->folder        = folder;
    entry->path          = path;
    entry->name          = ValueOr(keys, "Name", folder);
    entry->author        = ValueOr(keys, "Author", "");
    entry->version       = ValueOr(keys, "Version", "");
    entry->description   = ValueOr(keys, "Description", "");
    entry->hasLogic      = !ValueOr(keys, "LogicFile", "").empty();
    const std::string forced = ValueOr(keys, "ForceVersion", "0");
    const std::string target = forced != "0" ? forced : ValueOr(keys, "TargetVersion", "5");
    try {
        entry->targetVersion = std::stoi(target);
    }
    catch (const std::exception &) {
        entry->targetVersion = 5;
    }
    const auto measured = MeasureTree(path);
    entry->bytes        = measured.first;
    entry->files        = measured.second;
    return true;
}

std::vector<ModEntry> ModLibrary::Scan() const
{
    const std::vector<ModConfigEntry> config = ReadConfig();
    const std::vector<UsbDrive> drives       = ListUsbDrives();
    std::vector<std::pair<std::string, const UsbDrive *>> locations = { { TvFolder(), nullptr } };
    for (const UsbDrive &drive : drives)
        locations.emplace_back(UsbFolder(drive.mountPoint), &drive);

    std::vector<ModEntry> mods;
    std::set<std::string> pendingFolders, removingFolders, usedFolders;
    std::error_code ec;
    for (const auto &location : locations) {
        for (const fs::directory_entry &item : fs::directory_iterator(location.first, ec)) {
            const std::string name = item.path().filename().string();
            std::error_code itemError;
            if (EndsWith(name, kPendingSuffix))
                pendingFolders.insert(name.substr(0, name.size() - std::char_traits<char>::length(kPendingSuffix)));
            if (EndsWith(name, kDeleteSuffix))
                removingFolders.insert(name.substr(0, name.size() - std::char_traits<char>::length(kDeleteSuffix)));
            // On the TV, links to the drives' mods (LinkUsbMods()) are listed with their drive.
            if (fs::is_symlink(item.symlink_status(itemError)) || !item.is_directory(itemError) || !IsValidFolderName(name))
                continue;
            ModEntry entry;
            if (!ReadMod(item.path().string(), name, &entry))
                continue;
            entry.onUsb    = location.second != nullptr;
            entry.mount    = location.second ? location.second->mountPoint : std::string();
            entry.shadowed = !usedFolders.insert(name).second; // The TV's copy (scanned first) wins.
            mods.push_back(entry);
        }
    }

    for (ModEntry &mod : mods) {
        mod.pending  = pendingFolders.count(mod.folder) > 0;
        mod.removing = removingFolders.count(mod.folder) > 0;
        for (std::size_t i = 0; i < config.size(); ++i) {
            if (config[i].first == mod.folder) {
                mod.order  = static_cast<int>(i);
                mod.active = config[i].second;
                break;
            }
        }
    }
    std::stable_sort(mods.begin(), mods.end(), [](const ModEntry &a, const ModEntry &b) {
        if ((a.order < 0) != (b.order < 0))
            return a.order >= 0; // Listed ones first, as the engine loads them.
        if (a.order >= 0)
            return a.order < b.order;
        return a.name < b.name;
    });
    return mods;
}

std::vector<std::string> ModLibrary::ActiveFolders() const
{
    std::vector<std::string> active;
    for (const ModConfigEntry &entry : ReadConfig()) {
        if (entry.second)
            active.push_back(entry.first);
    }
    return active;
}

int ModLibrary::ApplyPending() const
{
    const std::vector<std::string> folders = Folders();
    int applied                            = 0;
    std::error_code ec;
    for (const std::string &location : folders) {
        for (const fs::directory_entry &item : fs::directory_iterator(location, ec)) {
            const std::string name = item.path().filename().string();
            std::error_code itemError;
            if (EndsWith(name, kDeleteSuffix) && item.is_regular_file(itemError)) {
                const std::string folder = name.substr(0, name.size() - std::char_traits<char>::length(kDeleteSuffix));
                if (IsValidFolderName(folder)) {
                    for (const std::string &other : folders)
                        RemovePath(fs::path(other) / folder);
                    ++applied;
                }
                fs::remove(item.path(), itemError);
            }
        }
    }
    for (const std::string &location : folders) {
        for (const fs::directory_entry &item : fs::directory_iterator(location, ec)) {
            const std::string name = item.path().filename().string();
            std::error_code itemError;
            if (!EndsWith(name, kPendingSuffix) || !item.is_directory(itemError))
                continue;
            const std::string folder = name.substr(0, name.size() - std::char_traits<char>::length(kPendingSuffix));
            if (!IsValidFolderName(folder))
                continue;
            // The replacement takes over from every other copy (a move's source among them).
            for (const std::string &other : folders)
                RemovePath(fs::path(other) / folder);
            fs::rename(item.path(), fs::path(location) / folder, itemError);
            applied += itemError ? 0 : 1;
        }
    }
    return applied;
}

void ModLibrary::LinkUsbMods() const
{
    const fs::path tv = TvFolder();
    std::error_code ec;
    fs::create_directories(tv, ec);
    // Start over: drives come and go between launches.
    for (const fs::directory_entry &item : fs::directory_iterator(tv, ec)) {
        std::error_code itemError;
        if (fs::is_symlink(item.symlink_status(itemError)))
            fs::remove(item.path(), itemError);
    }
    for (const UsbDrive &drive : ListUsbDrives()) {
        for (const fs::directory_entry &item : fs::directory_iterator(UsbFolder(drive.mountPoint), ec)) {
            const std::string name = item.path().filename().string();
            std::error_code itemError;
            if (!item.is_directory(itemError) || !IsValidFolderName(name) || !fs::is_regular_file(item.path() / kModIniName, itemError))
                continue;
            if (fs::exists(fs::symlink_status(tv / name, itemError)))
                continue; // The TV's own copy, or another drive's, wins.
            fs::create_directory_symlink(item.path(), tv / name, itemError);
            if (itemError)
                std::fprintf(stderr, "SonicWebMania: could not link the mod %s: %s\n", name.c_str(), itemError.message().c_str());
        }
    }
}

std::vector<ModConfigEntry> ModLibrary::ReadConfig() const
{
    std::vector<ModConfigEntry> entries;
    std::ifstream in(TvFolder() + "/" + kConfigName);
    std::string line;
    bool inMods = false;
    while (std::getline(in, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
            continue;
        if (trimmed.front() == '[') {
            inMods = trimmed == "[Mods]";
            continue;
        }
        const std::size_t equals = trimmed.find('=');
        if (!inMods || equals == std::string::npos)
            continue;
        const std::string folder = Trim(trimmed.substr(0, equals));
        const bool duplicate     = std::any_of(entries.begin(), entries.end(), [&folder](const ModConfigEntry &e) { return e.first == folder; });
        if (!folder.empty() && !duplicate)
            entries.emplace_back(folder, ParseBool(Trim(trimmed.substr(equals + 1))));
    }
    return entries;
}

bool ModLibrary::WriteConfig(const std::vector<ModConfigEntry> &entries, std::string *error) const
{
    std::error_code ec;
    fs::create_directories(TvFolder(), ec);
    const std::string path = TvFolder() + "/" + kConfigName;
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << "[Mods]\n";
        for (const ModConfigEntry &entry : entries)
            out << entry.first << '=' << (entry.second ? 'y' : 'n') << '\n';
        out.flush();
        if (!out) {
            if (error)
                *error = "Could not write " + path;
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec && error)
        *error = "Could not replace " + path + ": " + ec.message();
    return !ec;
}

bool ModLibrary::SetActive(const std::string &folder, bool active, std::string *error) const
{
    std::vector<ModConfigEntry> entries = ReadConfig();
    auto found = std::find_if(entries.begin(), entries.end(), [&folder](const ModConfigEntry &e) { return e.first == folder; });
    if (found == entries.end())
        entries.emplace_back(folder, active);
    else
        found->second = active;
    return WriteConfig(entries, error);
}

bool ModLibrary::MoveInOrder(const std::string &folder, int delta, std::string *error) const
{
    // Every mod gets a place first, in the order the page shows them.
    std::vector<ModConfigEntry> entries = ReadConfig();
    for (const ModEntry &mod : Scan()) {
        const bool listed = std::any_of(entries.begin(), entries.end(), [&mod](const ModConfigEntry &e) { return e.first == mod.folder; });
        if (!listed && !mod.shadowed)
            entries.emplace_back(mod.folder, false);
    }
    const auto found = std::find_if(entries.begin(), entries.end(), [&folder](const ModConfigEntry &e) { return e.first == folder; });
    if (found == entries.end()) {
        if (error)
            *error = "No such mod.";
        return false;
    }
    const std::ptrdiff_t from = found - entries.begin();
    const std::ptrdiff_t to   = from + delta;
    if (to >= 0 && to < static_cast<std::ptrdiff_t>(entries.size()))
        std::swap(entries[static_cast<std::size_t>(from)], entries[static_cast<std::size_t>(to)]);
    return WriteConfig(entries, error);
}

bool ModLibrary::Forget(const std::string &folder, std::string *error) const
{
    std::vector<ModConfigEntry> entries = ReadConfig();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&folder](const ModConfigEntry &e) { return e.first == folder; }),
                  entries.end());
    return WriteConfig(entries, error);
}

bool ModLibrary::IsValidFolderName(const std::string &folder)
{
    if (folder.size() > kMaxFolderBytes || folder.empty() || folder[0] == '.' || folder[0] == ' ' || !IsPortableName(folder))
        return false;
    if (folder.find_first_of("=;#[]") != std::string::npos)
        return false;
    for (const char *suffix : { kPartSuffix, kPendingSuffix, kDeleteSuffix }) {
        if (EndsWith(folder, suffix))
            return false;
    }
    return folder != kConfigName;
}

bool ModLibrary::IsValidRelativePath(const std::string &path)
{
    if (path.empty() || path.size() > kMaxPathBytes || path[0] == '/')
        return false;
    std::size_t depth = 0;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part[0] == '.' || !IsPortableName(part) || ++depth > kMaxPathDepth)
            return false;
        start = end + 1;
    }
    return true;
}

bool ModLibrary::CopyTree(const std::string &from, const std::string &to, const std::function<void(std::uint64_t)> &onProgress)
{
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec)
        return false;
    std::uint64_t copied = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path relative = fs::relative(it->path(), from, ec);
        if (ec)
            return false;
        const fs::path target = fs::path(to) / relative;
        std::error_code itemError;
        if (it->is_directory(itemError)) {
            fs::create_directories(target, itemError);
        }
        else if (it->is_regular_file(itemError)) {
            fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, itemError);
            copied += it->file_size(ec);
            if (onProgress)
                onProgress(copied);
        }
        if (itemError)
            return false;
    }
    return !ec;
}

std::pair<std::uint64_t, std::uint64_t> ModLibrary::MeasureTree(const std::string &folder)
{
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code itemError;
        if (it->is_regular_file(itemError)) {
            bytes += it->file_size(itemError);
            ++files;
        }
    }
    return { bytes, files };
}

} // namespace swm
