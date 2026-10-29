/**
 * @file IniEdit.h
 * @brief Rewrites a few keys of the engine's Settings.ini, keeping
 *        everything else the player changed.
 *
 * The app points the engine at its packs through Settings.ini (Game:dataFile
 * and friends). The packs may move between the TV and a USB drive between
 * two launches, so those keys are written again before every start.
 */
#ifndef SWM_INI_EDIT_H
#define SWM_INI_EDIT_H

#include <string>
#include <utility>
#include <vector>

namespace swm
{

/** Key/value pairs to set inside one ini section. */
using IniValues = std::vector<std::pair<std::string, std::string>>;

/**
 * Returns @p ini with the keys of @p values set inside section [@p section]:
 * existing lines are replaced in place, missing keys are added at the end of
 * the section, and the section is appended when the file has none. Keys are
 * compared without regard to case, as the engine's ini parser does.
 */
std::string SetIniValues(const std::string &ini, const std::string &section, const IniValues &values);

/**
 * Sets @p values in section [@p section] of the ini file at @p path (see
 * SetIniValues()), starting from @p defaultContent when the file is missing
 * or empty. The file is replaced atomically (written beside it, then renamed).
 *
 * @return False with a message in @p error if the file could not be written.
 */
bool UpdateIniFile(const std::string &path, const std::string &defaultContent, const std::string &section,
                   const IniValues &values, std::string *error);

} // namespace swm

#endif // SWM_INI_EDIT_H
