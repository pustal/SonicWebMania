/**
 * @file UsbDrives.cpp
 * @brief Lists the USB partitions webOS has mounted (see UsbDrives.h).
 */
#include "UsbDrives.h"

#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace swm
{
namespace
{

/** Where webOS mounts USB drives. */
constexpr const char *kUsbRoot = "/tmp/usb";

/** /proc/mounts writes spaces and a few other characters as \ooo octal escapes. */
std::string DecodeMountField(const std::string &field)
{
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            const std::string digits = field.substr(i + 1, 3);
            if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '7'; })) {
                out += static_cast<char>(std::strtol(digits.c_str(), nullptr, 8));
                i += 3;
                continue;
            }
        }
        out += field[i];
    }
    return out;
}

/** True when mount option list @p options (comma separated) contains "rw". */
bool HasReadWriteOption(const std::string &options)
{
    std::stringstream list(options);
    std::string option;
    while (std::getline(list, option, ',')) {
        if (option == "rw")
            return true;
    }
    return false;
}

/** Fills the space figures and writability of @p drive from its file system. */
void MeasureDrive(UsbDrive *drive, bool mountedReadWrite)
{
    struct statvfs info {};
    if (statvfs(drive->mountPoint.c_str(), &info) == 0) {
        drive->freeBytes  = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
        drive->totalBytes = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize;
    }
    drive->writable = mountedReadWrite && access(drive->mountPoint.c_str(), W_OK) == 0;
}

/** Folders directly inside @p dir, sorted. */
std::vector<std::filesystem::path> Subfolders(const std::filesystem::path &dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/**
 * $SWM_USB_ROOT stand-in: each <root>/<device>/<partition> folder counts as a
 * drive, mirroring the webOS layout without needing real mounts.
 */
std::vector<UsbDrive> ListStandInDrives(const std::string &root)
{
    std::vector<UsbDrive> drives;
    for (const std::filesystem::path &device : Subfolders(root)) {
        for (const std::filesystem::path &partition : Subfolders(device)) {
            UsbDrive drive;
            drive.mountPoint = partition.string();
            drive.name       = partition.filename().string();
            MeasureDrive(&drive, true);
            drives.push_back(drive);
        }
    }
    return drives;
}

} // namespace

std::vector<UsbDrive> ListUsbDrives()
{
    const char *standIn = std::getenv("SWM_USB_ROOT");
    if (standIn && *standIn)
        return ListStandInDrives(standIn);

    std::vector<UsbDrive> drives;
    std::ifstream mounts("/proc/mounts");
    std::string line;
    const std::string prefix = std::string(kUsbRoot) + "/";
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string device, mountPoint, fileSystem, options;
        if (!(fields >> device >> mountPoint >> fileSystem >> options))
            continue;

        mountPoint = DecodeMountField(mountPoint);
        if (mountPoint.compare(0, prefix.size(), prefix) != 0)
            continue;

        UsbDrive drive;
        drive.mountPoint = mountPoint;
        drive.name       = std::filesystem::path(mountPoint).filename().string();
        drive.fileSystem = fileSystem;
        MeasureDrive(&drive, HasReadWriteOption(options));
        drives.push_back(drive);
    }

    std::sort(drives.begin(), drives.end(), [](const UsbDrive &a, const UsbDrive &b) { return a.mountPoint < b.mountPoint; });
    // The same partition can appear twice (bind mounts); keep one entry per path.
    drives.erase(std::unique(drives.begin(), drives.end(),
                             [](const UsbDrive &a, const UsbDrive &b) { return a.mountPoint == b.mountPoint; }),
                 drives.end());
    return drives;
}

bool FindUsbDrive(const std::string &mountPoint, UsbDrive *out)
{
    for (const UsbDrive &drive : ListUsbDrives()) {
        if (drive.mountPoint == mountPoint) {
            if (out)
                *out = drive;
            return true;
        }
    }
    return false;
}

} // namespace swm
