/**
 * @file RemoteInput.h
 * @brief Maps LG remote control buttons onto the keyboard keys RSDK already understands.
 *
 * RSDK's default keyboard layout for player 1 is:
 *   arrows = D-pad, A = A (jump), S = B, D = C, Q = X, W = Y, E = Z,
 *   Return = Start, Tab = Select.
 * Rather than teaching the engine about the remote, each remote button is
 * rewritten into one of those keys before the engine sees the event. Game
 * controllers are untouched: the engine's SDL2 input device handles them.
 *
 * Layout (the remote held sideways, like a classic pad). The colour buttons
 * follow the Xbox controller's colours:
 *   2 = Left, 8 = Right, 6 = Up, 4 = Down,
 *   Green = A, Red = B, Blue = X, Yellow = Y, 0 = Start/Pause.
 * (In Sonic Mania both A and B jump, as they do on a controller.)
 * The remote's own arrow keys and OK button keep working as arrows and Start.
 */
#ifndef SWM_REMOTE_INPUT_H
#define SWM_REMOTE_INPUT_H

#include <SDL.h>

#include <string>

namespace swm
{

/** What the menu should do in response to an input event. */
enum class MenuAction {
    None,
    Up,
    Down,
    Confirm,
    Cancel,
};

namespace RemoteInput
{

/**
 * If @p event is a key event from a mapped remote button, rewrites its
 * scancode and keycode to the keyboard key the engine expects.
 *
 * @return True if the event was rewritten.
 */
bool RewriteForEngine(SDL_Event *event);

/** True when @p event is a press that should open the in-game menu (Back, Exit, Guide). */
bool IsMenuOpenRequest(const SDL_Event &event);

/** Translates a key or controller press into a menu action (remote, keyboard or pad). */
MenuAction ToMenuAction(const SDL_Event &event);

/** Applies the SDL hints that let a native webOS app receive Back/Exit and hide the pointer. */
void ApplyPlatformHints();

/**
 * Describes a key or controller press for the button test page, e.g.
 * "Red (code 486) -> B" or "Pad: a -> A". Returns an empty string for
 * events that are not presses.
 */
std::string DescribePress(const SDL_Event &event);

} // namespace RemoteInput
} // namespace swm

#endif // SWM_REMOTE_INPUT_H
