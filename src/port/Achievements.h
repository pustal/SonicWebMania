/**
 * @file Achievements.h
 * @brief The achievement data the Back menu shows.
 *
 * The engine already tracks Sonic Mania's achievements: it registers them
 * (RSDK::RegisterAchievement in UserCore.cpp), marks them when the game
 * unlocks one, and keeps the result in Achievements.bin next to the saves.
 * The port only reads that list. The engine side supplies a provider
 * function (see src/main.cpp), so this library needs no engine headers.
 */
#ifndef SWM_ACHIEVEMENTS_H
#define SWM_ACHIEVEMENTS_H

#include <string>
#include <vector>

namespace swm
{

/** One achievement as shown in the menu. */
struct AchievementEntry {
    std::string name;        ///< Display name, e.g. "Superstar".
    std::string description; ///< How to unlock it.
    bool unlocked;           ///< True once the game has awarded it.
};

/**
 * Returns the engine's current achievement list. Called on the main thread
 * while the menu is open (the engine is paused then), so no locking is needed.
 */
using AchievementProvider = std::vector<AchievementEntry> (*)();

} // namespace swm

#endif // SWM_ACHIEVEMENTS_H
