/**
 * @file RetroSaves.h
 * @brief What main() tells the retro games' save and mode support
 *        (src/RetroSaves.cpp) before and after the engine runs.
 */
#ifndef SWM_RETRO_SAVES_H
#define SWM_RETRO_SAVES_H

#include <string>

namespace swm
{

/**
 * Sets up the retro games' session before the engine starts.
 *
 * @param coinsPath Where Sonic Origins' coin count is kept, shared by the
 *                  retro games (an ini file with a "coins=" line).
 * @param gameId    The game about to run ("sonic1", "sonic2", "soniccd"; for
 *                  Sonic Mania nothing here is used), for its achievement list.
 */
void ConfigureRetroSession(const std::string &coinsPath, const std::string &gameId);

/** Saves what the session still holds (coins collected since the last stage). Call after the engine exits. */
void FinishRetroSession();

} // namespace swm

#endif // SWM_RETRO_SAVES_H
