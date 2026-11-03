/**
 * @file PlusFiles.h
 * @brief The engine Plus module's settings files, as seen from the launcher:
 *        reading them before the engine starts, and moving an earlier
 *        SonicWebMania's settings and saves into them.
 *
 * The engine (RSDKv5-Decompilation-Plus) keeps:
 *   - Plus.ini: maniaAudio, maniaSimilarMusic, maniaRemixes, lastGame, coins
 *     (Sonic Origins' coins, shared by the retro games), ending with "; end";
 *   - Settings.ini [Game] plusDLC: the official Plus DLCs, off by default;
 *   - retro/<game>/: each retro game's saves (SGame.bin, Achievements.bin).
 *
 * SonicWebMania 2.2 and earlier ran the retro games itself and kept:
 *   - port.ini: plusEnabled, retroEnabled, maniaAudio, maniaSimilarMusic,
 *     maniaRemixes, lastGame;
 *   - RetroCoins.ini: "coins=<n>";
 *   - games/<game>/: each retro game's Settings.ini, saves and log.
 * MigrateEarlierVersion() moves those over once.
 */
#ifndef SWM_PLUS_FILES_H
#define SWM_PLUS_FILES_H

#include "SettingsChannel.h"

namespace swm
{

class Storage;

/** Name of the engine Plus module's settings file, in the data directory. */
constexpr const char *kPlusIniName = "Plus.ini";

/** The settings saved in Plus.ini and Settings.ini (defaults for what is missing). */
GameSettings ReadSavedSettings(const Storage &storage);

/**
 * Moves an earlier version's settings and saves to where the engine looks
 * for them (see the file comment), when port.ini is there: Plus.ini is
 * written unless it exists, Settings.ini gets Game:plusDLC, the retro games'
 * saves move from games/<game>/ to retro/<game>/ (a save already there is
 * kept), and port.ini, RetroCoins.ini and games/ are removed.
 *
 * @param defaultSettings Settings.ini's content when it does not exist yet.
 * @return True when something was migrated.
 */
bool MigrateEarlierVersion(const Storage &storage, const char *defaultSettings);

} // namespace swm

#endif // SWM_PLUS_FILES_H
