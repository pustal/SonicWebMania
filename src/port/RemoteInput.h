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
 * Layout (the remote held sideways, like a classic pad):
 *   2 = Left, 8 = Right, 6 = Up, 4 = Down,
 *   Red = A (jump), Green = B, Yellow = X, Blue = Y, 0 = Start/Pause.
 * The remote's own arrow keys and OK button keep working as arrows and Start.
 */
#ifndef SWM_REMOTE_INPUT_H
#define SWM_REMOTE_INPUT_H

#include <SDL.h>

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

} // namespace RemoteInput
} // namespace swm

#endif // SWM_REMOTE_INPUT_H
