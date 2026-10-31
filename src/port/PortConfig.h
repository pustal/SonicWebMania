/**
 * @file PortConfig.h
 * @brief The port's own settings (port.ini), separate from the engine's Settings.ini.
 *
 * Values are read once at launch. Changes made through the web page are
 * written immediately but, like uploaded saves, only take effect after the
 * game restarts.
 */
#ifndef SWM_PORT_CONFIG_H
#define SWM_PORT_CONFIG_H

#include <mutex>
#include <string>

namespace swm
{

class PortConfig
{
public:
    /** File name inside the data directory. */
    static constexpr const char *kFileName = "port.ini";

    /** Loads @p path; missing files or keys keep their defaults. */
    explicit PortConfig(std::string path);

    /**
     * Whether the Mania Plus content is enabled. Off by default, as the
     * RSDKv5 decompilation licence requires for distributed builds.
     */
    bool IsPlusEnabled() const;

    /** Stores the Plus opt-in. @return False if port.ini could not be written. */
    bool SetPlusEnabled(bool enabled);

    /**
     * Whether the retro games (Sonic 1, 2 and CD from the player's own packs,
     * see RetroGames.h) are offered in the Back menu. Off by default.
     */
    bool IsRetroEnabled() const;

    /** Stores the retro opt-in. @return False if port.ini could not be written. */
    bool SetRetroEnabled(bool enabled);

    /**
     * Whether a retro game's missing music and sound effects come from Sonic
     * Mania's pack, when it is installed: the same sounds, and the tracks
     * Mania remixed (Green Hill, Chemical Plant...). On by default.
     */
    bool IsManiaAudioEnabled() const;

    /** Stores the Mania audio setting. @return False if port.ini could not be written. */
    bool SetManiaAudioEnabled(bool enabled);

    /**
     * Whether, on top of IsManiaAudioEnabled(), zones Mania has no version of
     * get a Mania track of a similar feel (Emerald Hill plays Green Hill...).
     * Off by default.
     */
    bool IsManiaSimilarMusicEnabled() const;

    /** Stores the similar-music setting. @return False if port.ini could not be written. */
    bool SetManiaSimilarMusicEnabled(bool enabled);

    /**
     * Whether, on top of IsManiaAudioEnabled(), the zones Mania remixed
     * (Green Hill, Chemical Plant, Oil Ocean, Stardust Speedway, Metallic
     * Madness) play Mania's remix even when the game's packs have the
     * original. Off by default.
     */
    bool IsManiaRemixesEnabled() const;

    /** Stores the Mania remixes setting. @return False if port.ini could not be written. */
    bool SetManiaRemixesEnabled(bool enabled);

    /**
     * The game to start when the app is opened: the id of the last game that
     * was left normally ("mania", "sonic1", ...), or "" for the default.
     *
     * main() clears it while a game runs and sets it again when the game is
     * left normally, so a game that crashes on start is not started again on
     * the next launch.
     */
    std::string LastGame() const;

    /** Stores the game to start next time ("" for the default). @return False if port.ini could not be written. */
    bool SetLastGame(const std::string &id);

private:
    /** Writes the current values to disk. Caller holds mutex_. */
    bool SaveLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    bool plusEnabled_  = false;
    bool retroEnabled_ = false;
    bool maniaAudio_   = true;
    bool maniaSimilar_ = false;
    bool maniaRemixes_ = false;
    std::string lastGame_;
};

} // namespace swm

#endif // SWM_PORT_CONFIG_H
