/**
 * @file RemoteInput.cpp
 * @brief Implementation of the remote control mapping (see RemoteInput.h).
 */
#include "RemoteInput.h"

#include <array>

namespace swm
{
namespace RemoteInput
{
namespace
{

/**
 * webOS remote scancodes added by the SDL-webOS backport (SDL_scancode.h,
 * SDL_SCANCODE_WEBOS_*). Spelled out numerically so desktop builds against
 * upstream SDL, which lacks these enumerators, still compile.
 */
constexpr int kWebOSBack   = 482;
constexpr int kWebOSRed    = 486;
constexpr int kWebOSGreen  = 487;
constexpr int kWebOSYellow = 488;
constexpr int kWebOSBlue   = 489;
constexpr int kWebOSExit   = 505;

/** One remote button and the keyboard key the engine should see instead. */
struct KeyMapping {
    int from;           ///< Incoming scancode (int: webOS codes are outside the SDL enum).
    SDL_Scancode to;    ///< Key from RSDK's default player-1 layout.
};

// clang-format off
/**
 * Remote held sideways: number pad becomes a D-pad, and the colour keys become
 * the face buttons of the same colour on an Xbox controller.
 */
constexpr std::array<KeyMapping, 16> kMappings = { {
    { SDL_SCANCODE_2,  SDL_SCANCODE_LEFT  }, { SDL_SCANCODE_KP_2, SDL_SCANCODE_LEFT  },
    { SDL_SCANCODE_8,  SDL_SCANCODE_RIGHT }, { SDL_SCANCODE_KP_8, SDL_SCANCODE_RIGHT },
    { SDL_SCANCODE_6,  SDL_SCANCODE_UP    }, { SDL_SCANCODE_KP_6, SDL_SCANCODE_UP    },
    { SDL_SCANCODE_4,  SDL_SCANCODE_DOWN  }, { SDL_SCANCODE_KP_4, SDL_SCANCODE_DOWN  },
    { SDL_SCANCODE_0,  SDL_SCANCODE_RETURN}, { SDL_SCANCODE_KP_0, SDL_SCANCODE_RETURN},
    { kWebOSGreen,     SDL_SCANCODE_A     }, // A (green on Xbox)
    { kWebOSRed,       SDL_SCANCODE_S     }, // B (red on Xbox)
    { kWebOSBlue,      SDL_SCANCODE_Q     }, // X (blue on Xbox)
    { kWebOSYellow,    SDL_SCANCODE_W     }, // Y (yellow on Xbox)
    { SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_RETURN },
    { SDL_SCANCODE_SELECT,   SDL_SCANCODE_RETURN }, // some remotes report OK as Select
} };
// clang-format on

bool IsKeyEvent(const SDL_Event &event) { return event.type == SDL_KEYDOWN || event.type == SDL_KEYUP; }

bool IsPress(const SDL_Event &event) { return event.type == SDL_KEYDOWN && !event.key.repeat; }

int ScancodeOf(const SDL_Event &event) { return static_cast<int>(event.key.keysym.scancode); }

} // namespace

bool RewriteForEngine(SDL_Event *event)
{
    if (!event || !IsKeyEvent(*event))
        return false;

    const int scancode = ScancodeOf(*event);
    for (const KeyMapping &mapping : kMappings) {
        if (mapping.from == scancode) {
            event->key.keysym.scancode = mapping.to;
            event->key.keysym.sym      = SDL_GetKeyFromScancode(mapping.to);
            return true;
        }
    }
    return false;
}

bool IsMenuOpenRequest(const SDL_Event &event)
{
    if (IsPress(event)) {
        const int scancode = ScancodeOf(event);
        return scancode == kWebOSBack || scancode == kWebOSExit || scancode == SDL_SCANCODE_ESCAPE
               || scancode == SDL_SCANCODE_AC_BACK;
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN)
        return event.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK;

    return false;
}

MenuAction ToMenuAction(const SDL_Event &event)
{
    if (event.type == SDL_KEYDOWN) {
        switch (ScancodeOf(event)) {
            case SDL_SCANCODE_UP:
            case SDL_SCANCODE_6:
            case SDL_SCANCODE_KP_6: return MenuAction::Up;

            case SDL_SCANCODE_DOWN:
            case SDL_SCANCODE_4:
            case SDL_SCANCODE_KP_4: return MenuAction::Down;

            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
            case SDL_SCANCODE_SELECT:
            case SDL_SCANCODE_0:
            case SDL_SCANCODE_KP_0:
            case kWebOSGreen: return MenuAction::Confirm; // A confirms, as on a controller

            case SDL_SCANCODE_ESCAPE:
            case SDL_SCANCODE_AC_BACK:
            case kWebOSBack:
            case kWebOSExit:
            case kWebOSRed: return event.key.repeat ? MenuAction::None : MenuAction::Cancel; // B backs out

            default: return MenuAction::None;
        }
    }

    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: return MenuAction::Up;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return MenuAction::Down;
            case SDL_CONTROLLER_BUTTON_A:
            case SDL_CONTROLLER_BUTTON_START: return MenuAction::Confirm;
            case SDL_CONTROLLER_BUTTON_B:
            case SDL_CONTROLLER_BUTTON_BACK:
            case SDL_CONTROLLER_BUTTON_GUIDE: return MenuAction::Cancel;
            default: return MenuAction::None;
        }
    }

    if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        constexpr Sint16 kThreshold = 24000;
        // Only the crossing matters; the menu ignores repeats while the stick is held.
        if (event.caxis.value < -kThreshold)
            return MenuAction::Up;
        if (event.caxis.value > kThreshold)
            return MenuAction::Down;
    }

    return MenuAction::None;
}

void ApplyPlatformHints()
{
#ifdef SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK
    // Deliver Back to the app instead of letting webOS treat it as "leave app".
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
#endif
#ifdef SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
#endif
#ifdef SDL_HINT_WEBOS_CURSOR_SLEEP_TIME
    // Hide the Magic Remote pointer quickly; the game has no pointer UI.
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "1000");
#endif
    // Controllers should keep working when another window has focus (e.g. a webOS toast).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
}

} // namespace RemoteInput
} // namespace swm
