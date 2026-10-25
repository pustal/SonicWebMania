/**
 * @file PortRuntime.h
 * @brief The in-game side of the port: implements the engine hooks declared in
 *        WebOSPortHooks.h, the Back-button menu (with the achievement list and
 *        a button test) and on-screen notifications.
 */
#ifndef SWM_PORT_RUNTIME_H
#define SWM_PORT_RUNTIME_H

#include "Achievements.h"
#include "BitmapFont.h"
#include "RemoteInput.h"
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

    /** @return True once "Restart" was chosen; main() relaunches the app after the engine exits. */
    bool RestartRequested() const { return restartRequested_; }

    // ---- Engine hooks (see WebOSPortHooks.h) ----

    void OnEngineVideoReady(SDL_Window *window, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice, WebOSPortRedrawFn redraw);
    bool FilterEvent(SDL_Event *event);
    void DrawOverlay(SDL_Renderer *renderer);
    bool ShouldQuit() const { return quitRequested_; }
    bool IsPlusEnabled() const { return plusEnabled_; }

private:
    /** Main menu entries, in display order. */
    enum class MenuItem { Resume, Achievements, ButtonTest, Restart, Close, Count };

    /** Which page of the menu is showing. */
    enum class Screen { Main, Achievements, ButtonTest };

    /** A remote key release held back so the game sees the press for at least one frame. */
    struct PendingRelease {
        SDL_Event event;
        std::uint32_t dueTicks;
    };

    PortRuntime() = default;

    /** Shows the menu and blocks the engine's main thread until it is closed. */
    void RunMenu();

    /** Handles one event while the menu is open. @return True when the menu should close. */
    bool HandleMenuEvent(const SDL_Event &event, bool *stickWasNeutral);

    /** Applies a navigation action on the main page. @return True when the menu should close. */
    bool HandleMainAction(MenuAction action);

    /** Applies a navigation action on the achievement list. */
    void HandleAchievementsAction(MenuAction action);

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
