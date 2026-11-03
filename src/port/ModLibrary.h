/**
 * @file ModLibrary.h
 * @brief Mods for the engine's mod loader, kept on the TV or on USB drives.
 *
 * The engine (RSDKv5-Decompilation-Plus) loads mods from the `mods` folder of
 * the data directory: one folder per mod, with a `mod.ini` (name, author,
 * version, the engine it targets: 5 Sonic Mania, 4 Sonic 1 and 2, 3 Sonic CD)
 * and the game files it replaces. `mods/modconfig.ini` lists which are on,
 * in priority order (when two mods change the same file, the first wins):
 *
 *     [Mods]
 *     SomeMod=y
 *     OtherMod=n
 *
 * The engine reads it whenever a game starts (at launch, and when the Plus
 * menu switches games); without its developer menu it never writes it, so
 * the web page owns it.
 *
 * Where mods are kept:
 *   - on the TV: `<data>/mods/<folder>/`;
 *   - on a USB drive: `<mount>/SonicWebMania/mods/<folder>/`. The engine only
 *     looks in its own folder, so at each launch LinkUsbMods() puts a link to
 *     each drive's mod there (a mod on the TV wins over one of the same name).
 *
 * The engine reads a mod's files while it plays, so a mod that was on at
 * launch is "in use" for the whole session and is never changed under it:
 *   - `<folder>.part/`: an upload or a move being written (left behind only
 *     when interrupted; the page's Clean up removes it);
 *   - `<folder>.pending/`: a replacement (an upload, or a move's copy) waiting
 *     for the next launch, when it replaces every other copy of the mod;
 *   - `<folder>.delete`: an empty file asking for the mod to go at the next launch.
 * ApplyPending() does both before the engine starts.
 *
 * Code mods (a `LogicFile` in mod.ini) are libraries built for one system;
 * those made for PCs do not load on the TV, and the page says so.
 */
#ifndef SWM_MOD_LIBRARY_H
#define SWM_MOD_LIBRARY_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace swm
{

class Storage;

/** A mod found in one of the mod folders. */
struct ModEntry {
    std::string folder;      ///< Folder name: the mod's id in modconfig.ini.
    std::string path;        ///< Absolute path of the folder.
    bool onUsb = false;      ///< On a USB drive rather than the TV.
    std::string mount;       ///< The drive's mount point, when onUsb.
    std::string name;        ///< mod.ini Name (the folder when missing).
    std::string author;      ///< mod.ini Author.
    std::string version;     ///< mod.ini Version.
    std::string description; ///< mod.ini Description.
    int targetVersion = 5;   ///< mod.ini TargetVersion (or ForceVersion): 5, 4, 3, or -1 for any.
    bool hasLogic     = false; ///< mod.ini names a LogicFile (code built for one system).
    std::uint64_t bytes = 0;   ///< Total size of its files.
    std::uint64_t files = 0;   ///< Number of files.
    bool active   = false;     ///< On in modconfig.ini.
    int order     = -1;        ///< Position in modconfig.ini (-1: not listed yet).
    bool pending  = false;     ///< A replacement waits for the next launch (`<folder>.pending` somewhere).
    bool removing = false;     ///< Removal waits for the next launch (`<folder>.delete`).
    bool shadowed = false;     ///< Another copy of the same folder (on the TV) is the one used.
};

/** One modconfig.ini line: a folder and whether it is on. */
using ModConfigEntry = std::pair<std::string, bool>;

class ModLibrary
{
public:
    /** The engine's mod folder, inside the data directory, and its list of mods. */
    static constexpr const char *kModsFolderName = "mods";
    static constexpr const char *kConfigName     = "modconfig.ini";
    /** The file that makes a folder a mod. */
    static constexpr const char *kModIniName = "mod.ini";

    /** Suffixes of the staging names (see the file comment); no mod folder may end with them. */
    static constexpr const char *kPartSuffix    = ".part";
    static constexpr const char *kPendingSuffix = ".pending";
    static constexpr const char *kDeleteSuffix  = ".delete";

    explicit ModLibrary(const Storage &storage);

    /** `<data>/mods`, where the engine looks. */
    std::string TvFolder() const;

    /** `<mount>/SonicWebMania/mods`. */
    static std::string UsbFolder(const std::string &mountPoint);

    /** Every mod folder: the TV's, then each connected drive's (whether it exists yet or not). */
    std::vector<std::string> Folders() const;

    /**
     * Every mod, in the order the engine loads them: those listed in
     * modconfig.ini first, in its order, then the others by name. Links
     * made by LinkUsbMods() are not listed twice.
     */
    std::vector<ModEntry> Scan() const;

    /** The mods on at launch, which the session then reads from (see the file comment). */
    std::vector<std::string> ActiveFolders() const;

    /** Applies the staged replacements and removals (see the file comment). @return How many. */
    int ApplyPending() const;

    /** Links each USB drive's mods into the TV's mod folder for the engine (see the file comment). */
    void LinkUsbMods() const;

    /** modconfig.ini's lines, in order. */
    std::vector<ModConfigEntry> ReadConfig() const;

    /** Replaces modconfig.ini. @return False with a message in @p error if it could not be written. */
    bool WriteConfig(const std::vector<ModConfigEntry> &entries, std::string *error) const;

    /** Turns mod @p folder on or off (adding it to modconfig.ini at the end when missing). */
    bool SetActive(const std::string &folder, bool active, std::string *error) const;

    /** Moves mod @p folder one place up (@p delta -1, higher priority) or down (+1). */
    bool MoveInOrder(const std::string &folder, int delta, std::string *error) const;

    /** Takes mod @p folder out of modconfig.ini. */
    bool Forget(const std::string &folder, std::string *error) const;

    /**
     * True for a folder name the engine, modconfig.ini and a USB drive's file
     * system all accept: printable, no path separators or characters FAT
     * refuses, none of ini's '=', ';', '#', '[' and ']', not hidden, at most
     * 64 bytes, and not ending in a staging suffix.
     */
    static bool IsValidFolderName(const std::string &folder);

    /**
     * True for a file's path inside a mod ("Data/Sprites/Players/Sonic.bin"):
     * relative, '/'-separated, no empty, "." or ".." part, nothing hidden,
     * names a USB drive can store, at most 240 bytes and 16 levels.
     */
    static bool IsValidRelativePath(const std::string &path);

    /**
     * Copies folder @p from to @p to (which must not exist), calling
     * @p onProgress with the bytes copied so far. @return False on any error
     * (what was copied is left for the caller to remove).
     */
    static bool CopyTree(const std::string &from, const std::string &to, const std::function<void(std::uint64_t)> &onProgress);

    /** Total size and number of files under @p folder. */
    static std::pair<std::uint64_t, std::uint64_t> MeasureTree(const std::string &folder);

private:
    /** Reads one mod folder's mod.ini into an entry (false when it has none). */
    static bool ReadMod(const std::string &path, const std::string &folder, ModEntry *entry);

    const Storage &storage_;
};

} // namespace swm

#endif // SWM_MOD_LIBRARY_H
