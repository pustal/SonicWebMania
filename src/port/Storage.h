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
 *
 * Sonic Mania's Data.rsdk may instead be kept on a USB drive, in a
 * "SonicWebMania" folder at the top of the drive (beside the retro games'
 * "retro" folder, see RetroGames.h). Only one copy is kept: the TV's wins if
 * both exist.
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
    /** Folder created at the top of a USB drive for the app's packs. */
    static constexpr const char *kUsbFolderName = "SonicWebMania";
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

    /** `<mount>/SonicWebMania`: the app's folder on a USB drive. */
    static std::string UsbFolder(const std::string &mountPoint);

    /** `<mount>/SonicWebMania/Data.rsdk`: where Sonic Mania's pack is kept on a USB drive. */
    static std::string UsbDataPackPath(const std::string &mountPoint);

    /** Where a Data.rsdk for the folder holding @p packPath waits for the next launch. */
    static std::string PendingPathFor(const std::string &packPath);

    /**
     * Sonic Mania's pack: Data.rsdk in the data directory if valid, otherwise
     * the first USB drive's. @return Its absolute path, or "" when there is none.
     */
    std::string DataPackPath() const;

    /** @return True when Sonic Mania's pack is installed (see DataPackPath()). */
    bool HasValidDataPack() const;

    /** @return True when the file at @p path starts with the "RSDK" signature. */
    static bool IsValidDataPackHeader(const std::string &path);

    /** Bytes available to the app on the data directory's filesystem. */
    std::uint64_t FreeBytes() const;

    /** Size of @p path in bytes, or 0 if it does not exist. */
    static std::uint64_t FileSize(const std::string &path);

    /**
     * Moves files staged during the previous session into place: a Data.rsdk
     * (on the TV or a USB drive; the other copies are then removed), saves
     * and settings for the data directory, and those staged under
     * incoming/games/<id>/ for a retro game's folder. Called before the engine
     * starts, so nothing has them open.
     *
     * @return Number of files applied.
     */
    int ApplyPendingFiles() const;

    /** Truncates the engine log when it has grown past kMaxLogBytes. */
    void TrimLog() const;

    /**
     * Saves and settings that may be downloaded or replaced through the web
     * page: those of the data directory, or of @p subdir inside it (a retro
     * game's working folder, "games/<id>").
     */
    std::vector<UserFile> ListUserFiles(const std::string &subdir = std::string()) const;

    /**
     * Whether @p name may be read or written through the web page: a plain
     * name (letters, digits, '-', '_') ending in .bin or .ini, never a path.
     */
    static bool IsAllowedUserFileName(const std::string &name);

    /** Folder (inside the data directory) holding the retro games' working folders; see RetroLibrary. */
    static constexpr const char *kGamesFolderName = "games";

    /** True for a game id: lower-case letters and digits only, at most 32. */
    static bool IsPlainName(const std::string &name);

private:
    std::string root_;
    bool persistent_;
};

} // namespace swm

#endif // SWM_STORAGE_H
