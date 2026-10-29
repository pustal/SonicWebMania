/**
 * @file Storage.cpp
 * @brief Implementation of the data directory helpers (see Storage.h).
 */
#include "Storage.h"

#include "PortConfig.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace swm
{
namespace
{

/** Name of the sub-directory used inside the app's home directory. */
constexpr const char *kDataFolderName = "data";
/** Fallback used when the home directory cannot be written (not persistent). */
constexpr const char *kVolatileFallback = "/tmp/sonicwebmania";
/** The engine's log file name (written by RSDK when logging is compiled in). */
constexpr const char *kEngineLogName = "log.txt";
/** Suffix of a Data.rsdk staged during play. */
constexpr const char *kPendingSuffix = ".pending";

/** Creates @p dir (and parents) and checks that a file can be written in it. */
bool EnsureWritableDirectory(const fs::path &dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::is_directory(dir))
        return false;

    const fs::path probe = dir / ".write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
    }
    fs::remove(probe, ec);
    return true;
}

/** Last write time of @p path as seconds since the Unix epoch (0 on error). */
std::int64_t ModificationTime(const fs::path &path)
{
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(path, ec);
    if (ec)
        return 0;

    // file_clock has no portable epoch in C++17; convert through the clocks' "now".
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

} // namespace

Storage Storage::Resolve()
{
    if (const char *overrideDir = std::getenv("SWM_DATA_DIR")) {
        if (*overrideDir && EnsureWritableDirectory(overrideDir))
            return Storage(overrideDir, true);
    }

    if (const char *home = std::getenv("HOME")) {
        // On webOS HOME points at the app's private, persistent data folder.
        // On a desktop build it is the user's home, so use a dedicated sub-folder.
#if defined(SWM_TARGET_WEBOS)
        const fs::path dir = fs::path(home) / kDataFolderName;
#else
        const fs::path dir = fs::path(home) / ".local" / "share" / "SonicWebMania";
#endif
        if (EnsureWritableDirectory(dir))
            return Storage(dir.string(), true);
    }

    EnsureWritableDirectory(kVolatileFallback);
    return Storage(kVolatileFallback, false);
}

Storage::Storage(std::string root, bool persistent) : root_(std::move(root)), persistent_(persistent)
{
    while (root_.size() > 1 && root_.back() == '/')
        root_.pop_back();
}

std::string Storage::PathFor(const std::string &name) const { return root_ + "/" + name; }

std::string Storage::IncomingDir() const { return PathFor("incoming"); }

std::string Storage::PendingDataPackPath() const { return PathFor(std::string(kDataPackName) + kPendingSuffix); }

bool Storage::HasValidDataPack() const { return IsValidDataPackHeader(PathFor(kDataPackName)); }

bool Storage::IsValidDataPackHeader(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    std::array<char, 4> signature{};
    in.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    return in.gcount() == static_cast<std::streamsize>(signature.size()) && signature[0] == 'R' && signature[1] == 'S'
           && signature[2] == 'D' && signature[3] == 'K';
}

std::uint64_t Storage::FreeBytes() const
{
    std::error_code ec;
    const fs::space_info info = fs::space(root_, ec);
    return ec ? 0 : static_cast<std::uint64_t>(info.available);
}

std::uint64_t Storage::FileSize(const std::string &path)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

int Storage::ApplyPendingFiles() const
{
    int applied = 0;
    std::error_code ec;

    // A replacement Data.rsdk uploaded during the previous session.
    const std::string pendingPack = PendingDataPackPath();
    if (fs::exists(pendingPack, ec)) {
        if (IsValidDataPackHeader(pendingPack)) {
            fs::rename(pendingPack, PathFor(kDataPackName), ec);
            applied += ec ? 0 : 1;
        }
        else {
            fs::remove(pendingPack, ec);
        }
    }

    // Saves and settings uploaded during the previous session: the data
    // directory's in incoming/, a retro game's in incoming/games/<id>/.
    const auto moveFiles = [&applied](const fs::path &from, const std::string &to) {
        std::error_code listError;
        for (const fs::directory_entry &entry : fs::directory_iterator(from, listError)) {
            const std::string name = entry.path().filename().string();
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || !IsAllowedUserFileName(name))
                continue;

            std::error_code moveError;
            fs::create_directories(to, moveError);
            fs::rename(entry.path(), to + "/" + name, moveError);
            applied += moveError ? 0 : 1;
        }
    };
    const fs::path incoming = IncomingDir();
    if (fs::is_directory(incoming, ec)) {
        moveFiles(incoming, root_);
        for (const fs::directory_entry &game : fs::directory_iterator(incoming / kGamesFolderName, ec)) {
            const std::string id = game.path().filename().string();
            std::error_code typeError;
            if (game.is_directory(typeError) && IsPlainName(id))
                moveFiles(game.path(), PathFor(std::string(kGamesFolderName) + "/" + id));
        }
    }

    return applied;
}

void Storage::TrimLog() const
{
    const std::string logPath = PathFor(kEngineLogName);
    if (FileSize(logPath) <= kMaxLogBytes)
        return;

    // Logs are only useful for the latest session; start over rather than rotating.
    std::ofstream truncate(logPath, std::ios::binary | std::ios::trunc);
}

std::vector<UserFile> Storage::ListUserFiles(const std::string &subdir) const
{
    std::vector<UserFile> files;
    std::error_code ec;

    const fs::path folder = subdir.empty() ? fs::path(root_) : fs::path(root_) / subdir;
    for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file() || !IsAllowedUserFileName(name))
            continue;

        files.push_back(UserFile{ name, FileSize(entry.path().string()), ModificationTime(entry.path()) });
    }

    std::sort(files.begin(), files.end(), [](const UserFile &a, const UserFile &b) { return a.name < b.name; });
    return files;
}

bool Storage::IsPlainName(const std::string &name)
{
    constexpr std::size_t kMaxNameLength = 32;
    return !name.empty() && name.size() <= kMaxNameLength
           && std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool Storage::IsAllowedUserFileName(const std::string &name)
{
    constexpr std::size_t kMaxNameLength = 64;
    if (name.empty() || name.size() > kMaxNameLength || name == PortConfig::kFileName)
        return false;

    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return false;

    const std::string extension = name.substr(dot);
    if (extension != ".bin" && extension != ".ini")
        return false;

    return std::all_of(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(dot), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

} // namespace swm
