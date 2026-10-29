/**
 * @file UsbDrives.h
 * @brief The USB drives webOS has mounted, for keeping large game data off
 *        the TV's own storage.
 *
 * webOS mounts USB partitions at /tmp/usb/<device>/<partition> (for example
 * /tmp/usb/sda/sda1). They are read from /proc/mounts, so only partitions that
 * are actually mounted are listed. $SWM_USB_ROOT replaces /tmp/usb, to try
 * the port on a computer with a folder standing in for a drive.
 */
#ifndef SWM_USB_DRIVES_H
#define SWM_USB_DRIVES_H

#include <cstdint>
#include <string>
#include <vector>

namespace swm
{

/** One mounted USB partition. */
struct UsbDrive {
    std::string mountPoint;      ///< Absolute path, e.g. "/tmp/usb/sda/sda1".
    std::string name;            ///< Partition name for people, e.g. "sda1".
    std::string fileSystem;      ///< As mounted, e.g. "ntfs", "vfat" ("" for $SWM_USB_ROOT folders).
    std::uint64_t freeBytes  = 0; ///< Space available to the app.
    std::uint64_t totalBytes = 0; ///< Size of the partition.
    bool writable            = false; ///< Mounted read-write and writable by the app.
};

/** The mounted USB partitions, sorted by mount point. Empty when there are none. */
std::vector<UsbDrive> ListUsbDrives();

/**
 * Looks up a drive by its mount point. Only paths returned by ListUsbDrives()
 * are accepted, so a web request cannot point the app at an arbitrary folder.
 *
 * @return True and fills @p out when @p mountPoint is a mounted USB partition.
 */
bool FindUsbDrive(const std::string &mountPoint, UsbDrive *out);

} // namespace swm

#endif // SWM_USB_DRIVES_H
