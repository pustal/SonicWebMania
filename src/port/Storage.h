/**
 * @file Storage.h
 * @brief Where the port keeps the game data, saves and its own settings, and
 *        how much room is left for them.
 *
 * Everything the engine reads or writes lives in one directory (the engine
 * uses the process working directory for Data.rsdk, Settings.ini and its
 * *.bin save files). Files uploaded while the game is running are staged in
 * an "incoming" sub-directory and only moved into place at the next launch,
 * so the engine never sees a file change underneath it.
 */
#ifndef SWM_STORAGE_H
#define SWM_STORAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace swm
{

/** A user-visible file (save, settings) in the data directory. */
struct UserFile {
    std::string name;   ///< File name without directory.
    std::uint64_t size; ///< Size in bytes.
    std::int64_t mtime; ///< Last modification, seconds since the Unix epoch.
};

class Storage
{
public:
    /** File name the engine opens for its game data. */
    static constexpr const char *kDataPackName = "Data.rsdk";
    /** Largest save or settings file accepted through the web page. */
    static constexpr std::uint64_t kMaxUserFileBytes = 4ull * 1024ull * 1024ull;
    /** Space kept free on top of an upload so the TV is never filled to the brim. */
    static constexpr std::uint64_t kSafetyMarginBytes = 32ull * 1024ull * 1024ull;
    /** The engine's log.txt is cut back to this size at every launch. */
    static constexpr std::uint64_t kMaxLogBytes = 256ull * 1024ull;

    /**
     * Picks the data directory: $SWM_DATA_DIR if set, otherwise a folder in
     * the app's home directory, falling back to /tmp when that is not writable.
     */
    static Storage Resolve();

    explicit Storage(std::string root, bool persistent);

    /** Absolute path of the data directory, without a trailing slash. */
    const std::string &Root() const { return root_; }

    /** False when the fallback (wiped on reboot) location had to be used. */
    bool IsPersistent() const { return persistent_; }

    /** Absolute path of @p name inside the data directory. */
    std::string PathFor(const std::string &name) const;

    /** Directory where files uploaded during play wait for the next launch. */
    std::string IncomingDir() const;

    /** Path a Data.rsdk uploaded during play is parked at until the next launch. */
    std::string PendingDataPackPath() const;

    /** @return True when Data.rsdk exists and starts with the RSDK signature. */
    bool HasValidDataPack() const;

    /** @return True when the file at @p path starts with the "RSDK" signature. */
    static bool IsValidDataPackHeader(const std::string &path);

    /** Bytes available to the app on the data directory's filesystem. */
    std::uint64_t FreeBytes() const;

    /** Size of @p path in bytes, or 0 if it does not exist. */
    static std::uint64_t FileSize(const std::string &path);

    /**
     * Moves files staged during the previous session into place. Called before
     * the engine starts, so nothing has them open.
     *
     * @return Number of files applied.
     */
    int ApplyPendingFiles() const;

    /** Truncates the engine log when it has grown past kMaxLogBytes. */
    void TrimLog() const;

    /** Saves and settings that may be downloaded or replaced through the web page. */
    std::vector<UserFile> ListUserFiles() const;

    /**
     * Whether @p name may be read or written through the web page: a plain
     * name (letters, digits, '-', '_') ending in .bin or .ini, never a path.
     */
    static bool IsAllowedUserFileName(const std::string &name);

private:
    std::string root_;
    bool persistent_;
};

} // namespace swm

#endif // SWM_STORAGE_H
