/**
 * @file RetroGames.h
 * @brief The "retro" games the engine's legacy RSDKv3/v4 cores can play:
 *        Sonic the Hedgehog, Sonic the Hedgehog 2 and Sonic CD, from the
 *        player's own data packs.
 *
 * Each game can have two packs:
 *   - **origins**: the .rsdk from Sonic Origins. It has the game but no music
 *     or sound effects (Origins plays those through its own engine);
 *   - **classic**: the Data.rsdk of the earlier standalone releases (the 2013
 *     remasters of Sonic 1 and 2, Sonic CD from 2011), sound included.
 *
 * How a game is played depends on which packs are there:
 *   - origins + classic: the Origins game, with the classic pack loaded second
 *     so its music and sound fill in what the Origins pack lacks
 *     (Settings.ini Game:extraDataFile, see patches/rsdk/0002). The sounds
 *     Origins added (the Drop Dash, Knuckles' and Amy's moves) stay silent;
 *   - origins only: the Origins game without sound;
 *   - classic only: the classic game, when its scripts are the mobile
 *     bytecode the engine runs (the 2013 Sonic 1 and 2 packs). Sonic CD's
 *     2011 PC pack has PC bytecode, so it can only lend its sound.
 *
 * Sonic CD's 2011 pack comes in an older format the engine cannot open; the
 * web page converts it on upload (see RsdkPack.h).
 *
 * Packs are looked for in the TV's data folder (`<data>/retro/`) and then in
 * a `SonicWebMania/retro/` folder on each USB drive, under fixed names:
 * `<game>-origins.rsdk` and `<game>-classic.rsdk` (for example
 * `sonic1-origins.rsdk`). The web page names uploads this way.
 *
 * Each game runs in its own working folder, `<data>/games/<game>/`, which
 * holds its Settings.ini, saves and log, so the games never share a save.
 */
#ifndef SWM_RETRO_GAMES_H
#define SWM_RETRO_GAMES_H

#include <cstdint>
#include <string>
#include <vector>

namespace swm
{

class Storage;

/** Which release a pack comes from. */
enum class RetroEdition {
    Origins, ///< Sonic Origins (no audio in the pack).
    Classic, ///< The earlier standalone release (with audio).
};

/** What a pack file on disk turned out to be. */
enum class PackState {
    Missing,     ///< No file.
    Ready,       ///< An RSDKv3/v4 pack the legacy engine can open.
    Unsupported, ///< Sonic CD's original 2011 pack format, which must be converted first (the web page does it).
    Invalid,     ///< An RSDK pack of the wrong kind (Sonic Mania's RSDKv5) or a damaged file.
};

/** One pack slot of a game. */
struct RetroPack {
    PackState state = PackState::Missing;
    std::string path;      ///< Absolute path when the file exists.
    std::uint64_t size = 0; ///< File size in bytes.
    char version    = 0;     ///< The pack's version byte ('3', '4', 'B') when Ready.
    bool onUsb      = false; ///< Found on a USB drive rather than the TV.
    bool standalone = false; ///< Ready and has bytecode the engine runs (see RsdkPack::HasEngineBytecode).
};

/** A game of the catalogue. */
struct RetroGameInfo {
    const char *id;    ///< Short id used in file names and URLs, e.g. "sonic1".
    const char *title; ///< Name shown to people.
};

/** The games this port knows how to set up. */
const std::vector<RetroGameInfo> &RetroCatalogue();

/** A game and the packs found for it. */
struct RetroGame {
    RetroGameInfo info{};
    RetroPack origins;
    RetroPack classic;

    /** True when the game can be started: an Origins pack, or a classic pack that runs on its own. */
    bool Playable() const { return origins.state == PackState::Ready || (classic.state == PackState::Ready && classic.standalone); }

    /** True when the game will have music and sound effects. */
    bool HasSound() const { return classic.state == PackState::Ready; }

    /** One-line description of how the game will be played, e.g. "Sonic Origins, sound from the classic release". */
    std::string ModeDescription() const;
};

class RetroLibrary
{
public:
    /** Folder (inside the data directory, or inside kUsbFolderName on a drive) holding the packs. */
    static constexpr const char *kPackFolderName = "retro";
    /** Folder created at the top of a USB drive. */
    static constexpr const char *kUsbFolderName = "SonicWebMania";
    /** Folder inside the data directory holding each game's working folder. */
    static constexpr const char *kGamesFolderName = "games";
    /** Id used for Sonic Mania itself in menus, port.ini and --swm-game. */
    static constexpr const char *kManiaId = "mania";

    explicit RetroLibrary(const Storage &storage);

    /** Looks for every game's packs on the TV and the USB drives. */
    std::vector<RetroGame> Scan() const;

    /** Scans and returns only the games that can be played. */
    std::vector<RetroGame> ScanPlayable() const;

    /** @return The catalogue entry for @p id, or null for an unknown id. */
    static const RetroGameInfo *FindGame(const std::string &id);

    /** "sonic1-origins.rsdk" and so on. */
    static std::string PackFileName(const std::string &id, RetroEdition edition);

    /** `<data>/retro`: where packs uploaded to the TV are kept. */
    std::string TvPackFolder() const;

    /** `<mount>/SonicWebMania/retro`: where packs are kept on a USB drive. */
    static std::string UsbPackFolder(const std::string &mountPoint);

    /** `<data>/games/<id>`: the game's working folder (settings, saves, log). */
    std::string WorkFolder(const std::string &id) const;

    /**
     * Creates the game's working folder and points its Settings.ini at the
     * packs found: Game:dataFile, Game:extraDataFile and Game:gameType are
     * rewritten at every launch (packs may have moved between the TV and a
     * drive); every other setting the player changed is kept. A new
     * Settings.ini starts from @p defaultSettings.
     *
     * @return False with a message in @p error if the folder or file could not be written.
     */
    bool PrepareLaunch(const RetroGame &game, const std::string &defaultSettings, std::string *error) const;

    /** Reads the start of @p path and classifies it (see PackState). */
    static PackState Inspect(const std::string &path, char *version);

private:
    /** Fills @p pack from the first folder in @p folders that has @p fileName. */
    static void FindPack(const std::vector<std::pair<std::string, bool>> &folders, const std::string &fileName, RetroPack *pack);

    const Storage &storage_;
};

/** Short lower-case name of a pack state for the web page ("missing", "ready", "unsupported", "invalid"). */
const char *PackStateName(PackState state);

} // namespace swm

#endif // SWM_RETRO_GAMES_H
