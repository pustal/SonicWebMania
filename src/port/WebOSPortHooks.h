/**
 * @file WebOSPortHooks.h
 * @brief The narrow C interface between the RSDKv5 engine and the webOS port layer.
 *
 * The engine is a git submodule that we patch as little as possible
 * (see patches/rsdk/). Every patched call site goes through one of the
 * functions declared here, so the engine never has to know about menus,
 * the remote control or the transfer server.
 *
 * All functions are called from the engine's main thread.
 */
#ifndef SWM_WEBOS_PORT_HOOKS_H
#define SWM_WEBOS_PORT_HOOKS_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback the port uses to redraw the last game frame (RSDK::RenderDevice::FlipScreen). */
typedef void (*WebOSPortRedrawFn)(void);

/**
 * Called once by the SDL2 render device after the window, renderer and audio
 * device exist, and before input devices are initialised.
 *
 * @param window      The engine's SDL window.
 * @param renderer    The engine's SDL renderer.
 * @param audioDevice The SDL audio device the engine plays through (0 if none).
 * @param redraw      Function that redraws the current frame and presents it.
 */
void WebOSPort_OnEngineVideoReady(SDL_Window *window, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice,
                                  WebOSPortRedrawFn redraw);

/**
 * Called for every SDL event before the engine sees it.
 *
 * The port may rewrite the event in place (for example a remote control
 * button becomes a keyboard key the engine understands) or consume it
 * entirely (for example the Back button opening the in-game menu).
 *
 * @param event The event to inspect; may be modified.
 * @return Non-zero if the event was consumed and the engine must ignore it.
 */
int WebOSPort_FilterEvent(SDL_Event *event);

/**
 * Called by the render device right before SDL_RenderPresent so the port can
 * draw its menu and notifications on top of the game frame.
 */
void WebOSPort_DrawOverlay(SDL_Renderer *renderer);

/** @return Non-zero once the user asked to close (or restart) the app. */
int WebOSPort_ShouldQuit(void);

/**
 * Whether the Mania Plus content may be enabled.
 *
 * The RSDKv5 decompilation licence requires distributed builds to have the
 * Plus DLC disabled by default; this returns the user's explicit opt-in.
 */
int WebOSPort_IsPlusEnabled(void);

/**
 * Called by the legacy (RSDKv3/v4) cores whenever a stage is about to load
 * (patches/rsdk/0003), with the engine version (3 or 4). Implemented in
 * src/RetroSaves.cpp: when a retro game leaves its title screen for its first
 * zone, it shows the save screen (Sonic 1 and 2) or the game's own menu
 * (Sonic CD), and may change the stage that loads.
 */
void WebOSPort_OnLegacyStageLoad(int engineVersion);

/**
 * Whether the stage is shown mirrored and left/right swapped: Sonic Origins'
 * Mirror mode, during play only (not on title screens or menus). Implemented
 * in src/RetroSaves.cpp; called by the legacy cores' drawing (which mirrors
 * the frame before the HUD and text are drawn) and the input code
 * (patches/rsdk/0004).
 */
int WebOSPort_IsMirrored(void);

#ifdef __cplusplus
}
#endif

#endif // SWM_WEBOS_PORT_HOOKS_H
