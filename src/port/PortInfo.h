/**
 * @file PortInfo.h
 * @brief App name, version and project page, shown in the menu, the setup
 *        screen and the transfer page.
 */
#ifndef SWM_PORT_INFO_H
#define SWM_PORT_INFO_H

namespace swm
{

/** Set by CMake from project(VERSION ...) so the app and the IPK always agree. */
#ifndef SWM_VERSION
#define SWM_VERSION "0.0.0-dev"
#endif

constexpr const char *kAppName    = "SonicWebMania";
constexpr const char *kAppVersion = SWM_VERSION;
/** Where people can find the source, issues and releases. */
constexpr const char *kProjectUrl = "https://github.com/pustal/SonicWebMania";
/** The same address without the scheme, for the small TV text. */
constexpr const char *kProjectUrlShort = "github.com/pustal/SonicWebMania";

} // namespace swm

#endif // SWM_PORT_INFO_H
