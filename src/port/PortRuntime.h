/**
 * @file PortRuntime.h
 * @brief The in-game side of the port: implements the engine hooks declared in
 *        WebOSPortHooks.h, the Back-button menu (with the achievement list and
 *        a button test), the retro games' save screen and on-screen
 *        notifications.
 */
#ifndef SWM_PORT_RUNTIME_H
#define SWM_PORT_RUNTIME_H

#include "Achievements.h"
#include "BitmapFont.h"
#include "RemoteInput.h"
#include "SaveSelect.h"
#include "WebOSPortHooks.h"

#include <SDL.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace swm
{

class TransferServer;

/** A game the Back menu's "Retro games" page can switch to. */
struct GameChoice {
    std::string id;    ///< "mania", "sonic1", ... (passed back to main() as the next game).
    std::string title; ///< Name shown in the list.
    std::string note;  ///< How it will play, shown under the selected entry (may be empty).
};

class PortRuntime
{
public:
    /** The one runtime the C hooks forward to. */
    static PortRuntime &Instance();

    /**
     * Supplies what the menu needs to show. Must be called before the engine starts.
     *
     * @param server       Transfer server (may be null if it failed to start).
     * @param lanAddress   Address shown for the web page ("" when offline).
     * @param plusEnabled  Whether Plus was opted into for this session.
     * @param achievements Reads the engine's achievement list (may be null).
     */
    void Configure(const TransferServer *server, std::string lanAddress, bool plusEnabled, AchievementProvider achievements);

    /**
     * Lists the games the player can switch to and which one is running. With
     * fewer than two games the menu has no "Retro games" entry.
     */
    void SetGames(std::vector<GameChoice> games, std::string currentId);

    /** @return True once "Restart" or another game was chosen; main() relaunches the app after the engine exits. */
    bool RestartRequested() const { return restartRequested_; }

    /** The game to relaunch into: the one chosen on the "Retro games" page, else the running one. */
    const std::string &NextGame() const { return nextGame_.empty() ? currentGame_ : nextGame_; }

    /**
     * Shows the save screen (the mode, then no save or four slots with
     * continue/new game/delete, then the character; see SaveSelect.h for which
     * pages a request has) over the frozen frame, and blocks the engine's main
     * thread until the player chooses. Called from src/RetroSaves.cpp when a
     * retro game leaves its title screen.
     *
     * @return The choice; Cancel when the player backed out or closed the app.
     */
    SaveSelectResult RunSaveSelect(const SaveSelectRequest &request);

    /**
     * Shows the achievement list on its own (Back or OK closes it), blocking
     * the engine's main thread. Sonic CD's menu opens it from its
     * Achievements entry.
     */
    void ShowAchievements();

    /** Shows @p text at the bottom of the screen for a few seconds (e.g. an unlocked achievement). */
    void Notify(const std::string &text);

    // ---- Engine hooks (see WebOSPortHooks.h) ----

    void OnEngineVideoReady(SDL_Window *window, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice, WebOSPortRedrawFn redraw);
    bool FilterEvent(SDL_Event *event);
    void DrawOverlay(SDL_Renderer *renderer);
    bool ShouldQuit() const { return quitRequested_; }
    bool IsPlusEnabled() const { return plusEnabled_; }

private:
    /** Main menu entries; menuItems_ holds the ones shown, in display order. */
    enum class MenuItem { Resume, Achievements, ButtonTest, Games, Restart, Close };

    /** Which page of the menu is showing (the last three are the save screen's). */
    enum class Screen { Main, Achievements, ButtonTest, Games, Modes, SaveSlots, SlotActions, Characters };

    /** Entries of the slot actions page, in display order. */
    enum class SlotAction { Continue, Delete, Back };

    /** A remote key release held back so the game sees the press for at least one frame. */
    struct PendingRelease {
        SDL_Event event;
        std::uint32_t dueTicks;
    };

    PortRuntime() = default;

    /** Shows the menu and blocks the engine's main thread until it is closed. */
    void RunMenu();

    /** The blocking loop behind RunMenu() and RunSaveSelect(), starting on page @p first. */
    void RunModal(Screen first);

    /** Navigation on the save screen's pages. @return True when the screen should close. */
    bool HandleModesAction(MenuAction action);
    bool HandleSaveSlotsAction(MenuAction action);
    bool HandleSlotActionsAction(MenuAction action);
    bool HandleCharactersAction(MenuAction action);

    /**
     * Moves on from the mode page: to the slots when the chosen mode saves,
     * else to the character page, else closes. @return True when the screen closed.
     */
    bool AfterModeChosen();

    /** Goes back one page from the slot or character page (closing from the first page). @return True when closed. */
    bool BackFromPage();

    /** Closes the save screen with @p action. @return True (for the handlers above). */
    bool FinishSaveSelect(SaveSelectAction action, int slot, int character);

    /** Handles one event while the menu is open. @return True when the menu should close. */
    bool HandleMenuEvent(const SDL_Event &event, bool *stickWasNeutral);

    /** Applies a navigation action on the main page. @return True when the menu should close. */
    bool HandleMainAction(MenuAction action);

    /** Applies a navigation action on the achievement list. */
    void HandleAchievementsAction(MenuAction action);

    /** Applies a navigation action on the game list. @return True when the menu should close. */
    bool HandleGamesAction(MenuAction action);

    /** Records a press on the button test page. Back/Exit/Guide return to the main page. */
    void HandleButtonTestEvent(const SDL_Event &event);

    /** Waits for the button that closed the menu to be released, so it does not reach the game. */
    void WaitForControllerRelease(SDL_JoystickID controller, SDL_GameControllerButton button);

    /** Re-queues held-back remote key releases whose time has come. */
    void FlushPendingReleases();

    /** Logical drawing size and integer text scale for the current renderer. */
    void Layout(SDL_Renderer *renderer, int *width, int *height, int *scale) const;

    /** Draws a bordered panel centred on screen and returns its rectangle. */
    SDL_Rect DrawPanel(int width, int height, int panelW, int panelH, int scale) const;

    void DrawMenu(int width, int height, int scale) const;
    void DrawMainPage(int width, int height, int scale) const;
    void DrawAchievementsPage(int width, int height, int scale) const;
    void DrawButtonTestPage(int width, int height, int scale) const;
    void DrawGamesPage(int width, int height, int scale) const;
    void DrawModesPage(int width, int height, int scale) const;
    void DrawSaveSlotsPage(int width, int height, int scale) const;
    void DrawSlotActionsPage(int width, int height, int scale) const;
    void DrawCharactersPage(int width, int height, int scale) const;

    /** Name of character @p id in the current save request ("" if unknown). */
    std::string CharacterName(int id) const;

    /**
     * Draws a list page: a title, @p rows (the selected one highlighted), up
     * to two wrapped lines of @p note under it and a @p footer line.
     */
    void DrawListPage(int width, int height, int scale, const std::string &title, const std::vector<std::string> &rows,
                      int selected, const std::string &note, const std::string &footer) const;

    /** Text of a main menu entry. */
    static const char *Label(MenuItem item);

    /** Rebuilds menuItems_ from the current game list. */
    void RebuildMenu();
    void DrawToast(int width, int height, int scale);

    /** "http://192.168.1.20:47801  PIN 1234", or a hint when the server is unavailable. */
    std::string TransferHint() const;

    const TransferServer *server_ = nullptr;
    std::string lanAddress_;
    bool plusEnabled_ = false;
    AchievementProvider achievementProvider_ = nullptr;

    SDL_Renderer *renderer_        = nullptr;
    SDL_AudioDeviceID audioDevice_ = 0;
    WebOSPortRedrawFn redraw_      = nullptr;
    TextRenderer text_;

    std::vector<MenuItem> menuItems_ = { MenuItem::Resume, MenuItem::Achievements, MenuItem::ButtonTest,
                                         MenuItem::Restart, MenuItem::Close };
    std::vector<GameChoice> games_;
    std::string currentGame_;
    std::string nextGame_;
    int gameSelected_ = 0;

    SaveSelectRequest saveRequest_;  ///< Shown by the save screen (deleted slots are cleared in place).
    SaveSelectResult saveResult_;    ///< Filled when the save screen closes.
    int saveRowSelected_      = 0;   ///< 0 = "No save", 1-4 = slots.
    int pendingSlot_          = -1;  ///< Slot the character page starts a game in (-1: no save).
    int slotActionSelected_   = 0;
    bool deleteArmed_         = false; ///< "Delete" was pressed once; a second press deletes.
    int characterSelected_    = 0;
    int modeSelected_         = 0;
    bool achievementsOnly_    = false; ///< The list was opened by ShowAchievements(): leaving it closes the menu.

    bool menuOpen_         = false;
    Screen screen_         = Screen::Main;
    int selected_          = 0;
    bool quitRequested_    = false;
    bool restartRequested_ = false;

    std::vector<AchievementEntry> achievements_; ///< Snapshot taken when the page opens.
    int achievementSelected_ = 0;
    int achievementTop_      = 0; ///< First visible row of the scrolling list.

    std::deque<std::string> buttonTestLog_; ///< Most recent presses first.

    /** When each engine-side key was last pressed by a remote button (indexed by scancode). */
    std::array<std::uint32_t, SDL_NUM_SCANCODES> remotePressTicks_{};
    std::vector<PendingRelease> pendingReleases_;

    std::uint32_t seenEventSerial_ = 0;
    std::string toastText_;
    std::uint32_t toastUntilTicks_ = 0;
};

} // namespace swm

#endif // SWM_PORT_RUNTIME_H
