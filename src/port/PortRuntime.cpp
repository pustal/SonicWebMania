/**
 * @file PortRuntime.cpp
 * @brief See PortRuntime.h.
 */
#include "PortRuntime.h"

#include "RemoteInput.h"

#include <algorithm>
#include <cstddef>

namespace swm
{
namespace
{

/**
 * Shortest time a remote button counts as held. The remote can report a
 * press and its release within one frame, and the engine reads each button
 * once per frame, so a release that early is held back until the game has
 * seen the press.
 */
constexpr std::uint32_t kMinRemotePressMs = 50;

} // namespace

PortRuntime &PortRuntime::Instance()
{
    static PortRuntime runtime;
    return runtime;
}

void PortRuntime::OnFrame(bool menuOpen)
{
    if (!filterInstalled_) {
        // SDL is up by the first frame; events it already queued are rare and harmless (a key the engine ignores).
        SDL_SetEventFilter(&PortRuntime::EventFilter, this);
        filterInstalled_ = true;
    }
    menuOpen_ = menuOpen;
    FlushPendingReleases();
}

int SDLCALL PortRuntime::EventFilter(void *userdata, SDL_Event *event)
{
    return static_cast<PortRuntime *>(userdata)->FilterEvent(event) ? 0 : 1;
}

bool PortRuntime::FilterEvent(SDL_Event *event)
{
    if (!event || (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP))
        return false;

    const auto from = static_cast<std::size_t>(event->key.keysym.scancode);
    if (RemoteInput::IsMenuKey(*event)) {
        if (from >= menuKeyTarget_.size())
            return false;
        if (event->type == SDL_KEYDOWN) {
            if (event->key.repeat)
                return true; // A held Back must not open and close the menu over and over.
            RemoteInput::RewriteMenuKey(event, menuOpen_);
            menuKeyTarget_[from] = event->key.keysym.scancode;
        }
        else {
            // Released as the key its press became, even if the menu opened in between.
            const SDL_Scancode target  = menuKeyTarget_[from] != SDL_SCANCODE_UNKNOWN ? menuKeyTarget_[from] : SDL_SCANCODE_TAB;
            event->key.keysym.scancode = target;
            event->key.keysym.sym      = SDL_GetKeyFromScancode(target);
            menuKeyTarget_[from]       = SDL_SCANCODE_UNKNOWN;
        }
    }
    else if (!RemoteInput::RewriteForEngine(event)) {
        return false; // A keyboard key, which the engine reads as it is.
    }

    // A remote button: make sure the game sees the press for at least a frame or two.
    const auto scancode = static_cast<std::size_t>(event->key.keysym.scancode);
    if (scancode >= remotePressTicks_.size())
        return false;

    const std::uint32_t now = SDL_GetTicks();
    if (event->type == SDL_KEYDOWN) {
        remotePressTicks_[scancode] = now;
        return false;
    }

    const std::uint32_t pressedAt = remotePressTicks_[scancode];
    if (now - pressedAt < kMinRemotePressMs) {
        pendingReleases_.push_back(PendingRelease{ *event, pressedAt + kMinRemotePressMs });
        return true; // Re-queued by FlushPendingReleases() once due.
    }
    return false;
}

void PortRuntime::FlushPendingReleases()
{
    if (pendingReleases_.empty())
        return;

    const std::uint32_t now = SDL_GetTicks();
    auto due = std::stable_partition(pendingReleases_.begin(), pendingReleases_.end(),
                                     [now](const PendingRelease &pending) { return !SDL_TICKS_PASSED(now, pending.dueTicks); });
    std::vector<PendingRelease> ready(due, pendingReleases_.end());
    pendingReleases_.erase(due, pendingReleases_.end());
    for (PendingRelease &pending : ready) {
        // Already an engine key, and its press time is cleared, so the filter lets it through.
        remotePressTicks_[static_cast<std::size_t>(pending.event.key.keysym.scancode)] = 0;
        SDL_PushEvent(&pending.event);
    }
}

} // namespace swm
