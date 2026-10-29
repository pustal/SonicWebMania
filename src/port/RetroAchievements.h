/**
 * @file RetroAchievements.h
 * @brief The achievements of the retro games, in the order their scripts
 *        number them.
 *
 * The scripts unlock achievements by number (SetAchievement id, 100). The
 * numbers are those of the earlier releases, and the Sonic Origins packs use
 * the same ones for Sonic 1 and 2. The engine has no names for them, so the
 * port supplies them:
 *   - Sonic 1 and 2: the 2013 releases' lists (as in RSDKv4-Decompilation's
 *     RetroEngine.cpp);
 *   - Sonic CD: the 2011 release's list (as in RSDKv3-Decompilation's
 *     Userdata.cpp).
 */
#ifndef SWM_RETRO_ACHIEVEMENTS_H
#define SWM_RETRO_ACHIEVEMENTS_H

#include <string>
#include <vector>

namespace swm
{

/** One achievement: shown in the Back menu's list. */
struct RetroAchievement {
    const char *name;
    const char *description;
};

/** The achievements of retro game @p gameId ("sonic1", "sonic2", "soniccd"); empty for any other id. */
const std::vector<RetroAchievement> &RetroAchievementsFor(const std::string &gameId);

} // namespace swm

#endif // SWM_RETRO_ACHIEVEMENTS_H
