/**
 * @file PortConfig.cpp
 * @brief Reads and writes port.ini (see PortConfig.h). The format is plain
 *        "key=value" lines (plusEnabled, retroEnabled, lastGame); unknown
 *        keys are ignored.
 */
#include "PortConfig.h"

#include <fstream>

namespace swm
{
namespace
{

constexpr const char *kPlusKey     = "plusEnabled";
constexpr const char *kRetroKey    = "retroEnabled";
constexpr const char *kLastGameKey = "lastGame";

/** Game ids are short names of letters and digits; anything else is ignored. */
bool IsPlainId(const std::string &id)
{
    if (id.empty() || id.size() > 32)
        return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/** Interprets the usual spellings of a boolean ini value. */
bool ParseBool(const std::string &value) { return value == "1" || value == "true" || value == "yes" || value == "y"; }

/** Removes leading and trailing blanks. */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

PortConfig::PortConfig(std::string path) : path_(std::move(path))
{
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || line[0] == '#' || line[0] == ';')
            continue;

        const std::string key   = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));
        if (key == kPlusKey)
            plusEnabled_ = ParseBool(value);
        else if (key == kRetroKey)
            retroEnabled_ = ParseBool(value);
        else if (key == kLastGameKey && IsPlainId(value))
            lastGame_ = value;
    }
}

bool PortConfig::IsPlusEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return plusEnabled_;
}

bool PortConfig::SetPlusEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    plusEnabled_ = enabled;
    return SaveLocked();
}

bool PortConfig::IsRetroEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return retroEnabled_;
}

bool PortConfig::SetRetroEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    retroEnabled_ = enabled;
    return SaveLocked();
}

std::string PortConfig::LastGame() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastGame_;
}

bool PortConfig::SetLastGame(const std::string &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastGame_ = IsPlainId(id) ? id : std::string();
    return SaveLocked();
}

bool PortConfig::SaveLocked() const
{
    std::ofstream out(path_, std::ios::trunc);
    if (!out)
        return false;

    out << "# SonicWebMania port settings (changes apply after a restart)\n";
    out << kPlusKey << '=' << (plusEnabled_ ? "1" : "0") << '\n';
    out << kRetroKey << '=' << (retroEnabled_ ? "1" : "0") << '\n';
    if (!lastGame_.empty())
        out << kLastGameKey << '=' << lastGame_ << '\n';
    return static_cast<bool>(out);
}

} // namespace swm
