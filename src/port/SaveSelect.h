/**
 * @file SaveSelect.h
 * @brief What the Back-menu runtime's save screen shows and returns, for
 *        the retro games, independent of the engine.
 *
 * The 2013 releases of Sonic 1 and 2 drew their save slots in native code
 * and Sonic Origins draws them in its own program; with only the game data,
 * the engine goes from the title screen straight into the first zone. The
 * port fills that gap: src/RetroSaves.cpp (which knows the engine) reads the
 * game's save memory into a SaveSelectRequest, PortRuntime::RunSaveSelect()
 * shows it over the frozen title screen, and the result is applied back.
 *
 * The screen's pages, each shown only when the request has something for it:
 *   1. Mode (Sonic Origins packs): Anniversary, Classic, Mirror, Boss Rush;
 *   2. Slots: no save, or one of four slots (continue, new game, delete);
 *   3. Character, for a new game.
 */
#ifndef SWM_SAVE_SELECT_H
#define SWM_SAVE_SELECT_H

#include <array>
#include <string>
#include <vector>

namespace swm
{

/** Number of save slots the games' save memory holds. */
constexpr int kSaveSlotCount = 4;

/** One save slot as shown on the screen. */
struct RetroSaveSlot {
    bool used = false;   ///< Holds a game in progress (or a cleared game).
    int character = 0;   ///< Character id (see RetroCharacter::id).
    std::string place;   ///< Where play continues, e.g. "Emerald Hill Zone 2".
    int lives = 0;
    int emeralds = 0;    ///< Number of Chaos Emeralds collected.
    bool cleared = false; ///< The game was finished (continues at the post-game stage menu).
};

/** A character the game can be started with. */
struct RetroCharacter {
    int id = 0;       ///< The engine's player list position (0 Sonic, 1 Tails, ...).
    std::string name; ///< Shown in the list.
};

/** A way to play the game (Sonic Origins' modes). */
struct RetroMode {
    std::string name;        ///< Shown in the list, e.g. "Anniversary".
    std::string description; ///< Shown under the selected entry.
    bool usesSaves = true;   ///< Goes on to the save slots (Boss Rush does not).
};

/** Everything the save screen shows. */
struct SaveSelectRequest {
    std::string title; ///< The game's name.
    std::vector<RetroMode> modes; ///< Empty: no mode page.
    bool slotsEnabled = true;     ///< False: no slot page (the game has its own menu).
    std::array<RetroSaveSlot, kSaveSlotCount> slots{};
    std::vector<RetroCharacter> characters; ///< Empty: no character page.
};

/** How the save screen ended. */
enum class SaveSelectAction {
    Cancel,   ///< Back: return to the title screen.
    NoSave,   ///< Play without saving (also: a mode was chosen where there are no slots).
    NewGame,  ///< Start a new game in `slot`.
    Continue, ///< Continue the game in `slot`.
};

struct SaveSelectResult {
    SaveSelectAction action = SaveSelectAction::Cancel;
    int slot      = -1; ///< 0-based slot for NewGame / Continue.
    int character = 0;  ///< Character id for NoSave / NewGame.
    int mode      = 0;  ///< Index into SaveSelectRequest::modes (0 when there are none).
    std::vector<int> deleted; ///< Slots the player deleted on the screen (apply even on Cancel).
};

} // namespace swm

#endif // SWM_SAVE_SELECT_H
