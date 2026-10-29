/**
 * @file SetupScreen.h
 * @brief First-run screen shown while no game data is installed.
 *
 * It tells the user which address to open on a phone or computer, shows the
 * PIN, the free space on the TV and the upload progress, and returns as soon
 * as a valid Data.rsdk has arrived, or a retro game (see RetroGames.h) can
 * be played.
 */
#ifndef SWM_SETUP_SCREEN_H
#define SWM_SETUP_SCREEN_H

#include <functional>

namespace swm
{

class Storage;
class TransferServer;

/** How the setup screen ended. */
enum class SetupResult {
    DataReady, ///< A valid Data.rsdk is installed, or a retro game can be played; start.
    Exit,      ///< The user closed the app.
    Failed,    ///< The screen could not be created.
};

/**
 * Runs the setup screen until the data arrives or the user exits.
 * Creates and destroys its own window; SDL video is shut down on return so
 * the engine can initialise it from scratch.
 *
 * @param server         May be null if the transfer server failed to start.
 * @param otherGameReady Polled every couple of seconds: true when a retro game
 *                       became playable (enabled and its pack uploaded). May be empty.
 */
SetupResult RunSetupScreen(const Storage &storage, const TransferServer *server, const std::function<bool()> &otherGameReady);

} // namespace swm

#endif // SWM_SETUP_SCREEN_H
