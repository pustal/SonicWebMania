/**
 * @file PortRuntime.h
 * @brief The in-game side of the port: implements the engine hooks declared in
 *        WebOSPortHooks.h, the Back-button menu and on-screen notifications.
 */
#ifndef SWM_PORT_RUNTIME_H
#define SWM_PORT_RUNTIME_H

#include "BitmapFont.h"
#include "RemoteInput.h"
#include "WebOSPortHooks.h"

#include <SDL.h>

#include <cstdint>
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
     */
    void Configure(const TransferServer *server, std::string lanAddress, bool plusEnabled);

    /** @return True once "Restart" was chosen; main() relaunches the app after the engine exits. */
    bool RestartRequested() const { return restartRequested_; }

    // ---- Engine hooks (see WebOSPortHooks.h) ----

    void OnEngineVideoReady(SDL_Window *window, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice, WebOSPortRedrawFn redraw);
    bool FilterEvent(SDL_Event *event);
    void DrawOverlay(SDL_Renderer *renderer);
    bool ShouldQuit() const { return quitRequested_; }
    bool IsPlusEnabled() const { return plusEnabled_; }

private:
    /** Menu entries, in display order. */
    enum class MenuItem { Resume, Restart, Close, Count };

    PortRuntime() = default;

    /** Shows the menu and blocks the engine's main thread until it is closed. */
    void RunMenu();

    /** Applies one menu action. @return True when the menu should close. */
    bool HandleMenuAction(MenuAction action);

    /** Waits for the button that closed the menu to be released, so it does not reach the game. */
    void WaitForControllerRelease(SDL_JoystickID controller, SDL_GameControllerButton button);

    /** Logical drawing size and integer text scale for the current renderer. */
    void Layout(SDL_Renderer *renderer, int *width, int *height, int *scale) const;

    void DrawMenu(int width, int height, int scale) const;
    void DrawToast(int width, int height, int scale);

    /** "http://192.168.1.20:47801  PIN 1234", or a hint when the server is unavailable. */
    std::string TransferHint() const;

    const TransferServer *server_ = nullptr;
    std::string lanAddress_;
    bool plusEnabled_ = false;

    SDL_Renderer *renderer_       = nullptr;
    SDL_AudioDeviceID audioDevice_ = 0;
    WebOSPortRedrawFn redraw_     = nullptr;
    TextRenderer text_;

    bool menuOpen_          = false;
    int selected_           = 0;
    bool quitRequested_     = false;
    bool restartRequested_  = false;

    std::uint32_t seenEventSerial_ = 0;
    std::string toastText_;
    std::uint32_t toastUntilTicks_ = 0;
};

} // namespace swm

#endif // SWM_PORT_RUNTIME_H
