/**
 * @file SettingsChannel.h
 * @brief Hands the game settings between the transfer server's threads and
 *        the engine's thread.
 *
 * The settings themselves belong to the engine's Plus module (Plus.ini, and
 * Settings.ini Game:plusDLC), which also changes them from its own menu and
 * may only be called on the engine's thread. So the web page never writes
 * those files: a change it asks for waits here until the engine's next frame
 * applies it (main.cpp), and the engine publishes what it has every frame,
 * for the page to show. Before the engine runs (the setup screen), the
 * values read from the files at launch are shown, and a change waits for
 * the engine to start.
 *
 * This header does not depend on the engine, so the port library stays
 * independent of it; main.cpp converts to and from RSDK::Plus::Settings.
 */
#ifndef SWM_SETTINGS_CHANNEL_H
#define SWM_SETTINGS_CHANNEL_H

#include <mutex>

namespace swm
{

/** The settings the web page offers (see RSDK::Plus::Settings). */
struct GameSettings {
    bool plusDLC      = false; ///< The official Plus DLCs, for those who own them. Off by default (decompilation licence).
    bool maniaAudio   = true;  ///< Sonic Mania's audio fills the retro games' gaps.
    bool similarMusic = false; ///< ...including similar music for zones Mania has no version of.
    bool maniaRemixes = false; ///< The zones Mania remixed play the remix even when the packs have the original.

    bool operator==(const GameSettings &other) const
    {
        return plusDLC == other.plusDLC && maniaAudio == other.maniaAudio && similarMusic == other.similarMusic
               && maniaRemixes == other.maniaRemixes;
    }
    bool operator!=(const GameSettings &other) const { return !(*this == other); }
};

class SettingsChannel
{
public:
    /** @param initial The settings as saved, until the engine publishes its own. */
    explicit SettingsChannel(const GameSettings &initial) : current_(initial) {}

    /** What the page should show: the change waiting to be applied, else the engine's settings. Any thread. */
    GameSettings Current() const;

    /** Asks for @p settings to be applied on the engine's next frame. Any thread. */
    void Request(const GameSettings &settings);

    /** Takes the change waiting, if any. Engine thread. @return False when there is none. */
    bool TakeRequest(GameSettings *settings);

    /** Records the engine's settings (each frame, after any change was applied). Engine thread. */
    void Publish(const GameSettings &settings);

private:
    mutable std::mutex mutex_;
    GameSettings current_;
    GameSettings requested_;
    bool hasRequest_ = false;
};

} // namespace swm

#endif // SWM_SETTINGS_CHANNEL_H
