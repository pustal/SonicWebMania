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

private:
    /** Writes the current values to disk. Caller holds mutex_. */
    bool SaveLocked() const;

    std::string path_;
    mutable std::mutex mutex_;
    bool plusEnabled_ = false;
};

} // namespace swm

#endif // SWM_PORT_CONFIG_H
