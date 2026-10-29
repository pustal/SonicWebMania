/**
 * @file IniEdit.cpp
 * @brief Implementation of the Settings.ini rewriting (see IniEdit.h).
 */
#include "IniEdit.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace swm
{
namespace
{

/** Removes leading and trailing blanks. */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/** Reads a whole text file ("" when it does not exist). */
std::string ReadText(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

std::string SetIniValues(const std::string &ini, const std::string &section, const IniValues &values)
{
    auto lower = [](std::string text) {
        for (char &c : text)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return text;
    };

    std::vector<std::string> lines;
    {
        std::istringstream in(ini);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    std::vector<bool> written(values.size(), false);
    const std::string wantedSection = lower(section);
    bool inSection                  = false;
    std::size_t sectionEnd          = std::string::npos; // Index after the last line of the section.

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(lines[i]);
        if (!trimmed.empty() && trimmed.front() == '[') {
            const std::size_t close = trimmed.find(']');
            inSection               = close != std::string::npos && lower(Trim(trimmed.substr(1, close - 1))) == wantedSection;
            if (inSection)
                sectionEnd = i + 1;
            continue;
        }
        if (!inSection)
            continue;
        if (!trimmed.empty())
            sectionEnd = i + 1;

        const std::size_t equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = lower(Trim(trimmed.substr(0, equals)));
        for (std::size_t v = 0; v < values.size(); ++v) {
            if (key == lower(values[v].first)) {
                lines[i]   = values[v].first + "=" + values[v].second;
                written[v] = true;
            }
        }
    }

    std::vector<std::string> missing;
    for (std::size_t v = 0; v < values.size(); ++v) {
        if (!written[v])
            missing.push_back(values[v].first + "=" + values[v].second);
    }
    if (sectionEnd == std::string::npos) {
        if (!lines.empty() && !Trim(lines.back()).empty())
            lines.emplace_back();
        lines.push_back("[" + section + "]");
        lines.insert(lines.end(), missing.begin(), missing.end());
    }
    else {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(sectionEnd), missing.begin(), missing.end());
    }

    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

bool UpdateIniFile(const std::string &path, const std::string &defaultContent, const std::string &section,
                   const IniValues &values, std::string *error)
{
    std::string ini = ReadText(path);
    if (Trim(ini).empty())
        ini = defaultContent;
    ini = SetIniValues(ini, section, values);

    const std::string partPath = path + ".part";
    {
        std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
        out << ini;
        if (!out) {
            if (error)
                *error = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(partPath, path, ec);
    if (ec) {
        if (error)
            *error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace swm
