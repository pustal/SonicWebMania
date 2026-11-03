/**
 * @file PortRuntime.h
 * @brief The in-game side of the port: the LG remote's buttons, rewritten
 *        into the keys the engine reads.
 *
 * Everything else in the game (the Plus menu opened with Back, the retro
 * games, their save screen, achievements, notices) is the engine's Plus
 * module (RSDKv5-Decompilation-Plus, RSDK/Plus/). The engine needs no change
 * for this: the rewriting is an SDL event filter, installed once SDL runs.
 */
#ifndef SWM_PORT_RUNTIME_H
#define SWM_PORT_RUNTIME_H

#include <SDL.h>

#include <array>
#include <cstdint>
#include <vector>

namespace swm
{

class PortRuntime
{
public:
    /** The one runtime the SDL event filter forwards to. */
    static PortRuntime &Instance();

    /**
     * Once per frame, on the engine's thread (main.cpp's frame callback):
     * installs the event filter the first time, notes whether the Plus menu
     * is open (@p menuOpen: Back then goes back a page instead of opening
     * it) and hands over remote button releases held back so far.
     */
    void OnFrame(bool menuOpen);

private:
    /** A remote key release held back so the game sees the press for at least one frame. */
    struct PendingRelease {
        SDL_Event event;
        std::uint32_t dueTicks;
    };

    PortRuntime() = default;

    /** SDL_SetEventFilter() callback. @return 0 to drop the event. */
    static int SDLCALL EventFilter(void *userdata, SDL_Event *event);

    /** Rewrites a remote button into an engine key. @return True to drop the event (held back for later). */
    bool FilterEvent(SDL_Event *event);

    /** Re-queues held-back remote key releases whose time has come. */
    void FlushPendingReleases();

    bool filterInstalled_ = false;
    bool menuOpen_        = false;

    /** When each engine-side key was last pressed by a remote button (indexed by scancode). */
    std::array<std::uint32_t, SDL_NUM_SCANCODES> remotePressTicks_{};
    /** The engine key each menu key's press became (Tab or S), so its release matches it. */
    std::array<SDL_Scancode, SDL_NUM_SCANCODES> menuKeyTarget_{};
    std::vector<PendingRelease> pendingReleases_;
};

} // namespace swm

#endif // SWM_PORT_RUNTIME_H
