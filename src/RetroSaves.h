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
 * Where Sonic Origins' coin count is kept, shared by the retro games (an
 * ini file with a "coins=" line). Call before the engine starts.
 */
void ConfigureRetroSession(const std::string &coinsPath);

/** Saves what the session still holds (coins collected since the last stage). Call after the engine exits. */
void FinishRetroSession();

} // namespace swm

#endif // SWM_RETRO_SAVES_H
