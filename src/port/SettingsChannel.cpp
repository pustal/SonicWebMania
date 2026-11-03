/**
 * @file SettingsChannel.cpp
 * @brief See SettingsChannel.h.
 */
#include "SettingsChannel.h"

namespace swm
{

GameSettings SettingsChannel::Current() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hasRequest_ ? requested_ : current_;
}

void SettingsChannel::Request(const GameSettings &settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    requested_  = settings;
    hasRequest_ = true;
}

bool SettingsChannel::TakeRequest(GameSettings *settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasRequest_)
        return false;
    *settings   = requested_;
    hasRequest_ = false;
    return true;
}

void SettingsChannel::Publish(const GameSettings &settings)
{
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = settings;
}

} // namespace swm
