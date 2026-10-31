/**
 * @file TransferServer.cpp
 * @brief Implementation of the asset/save transfer server (see TransferServer.h).
 *
 * HTTP API (all /api routes require the PIN, as an "X-Pin" header or a ?pin= query):
 *   GET  /                    the upload page (web/index.html, embedded at build time)
 *   GET  /api/status          JSON: mode, free space, data pack state, settings
 *   PUT  /api/data?dest=tv|usb[&mount=<USB mount point>]
 *                             raw body = Sonic Mania's Data.rsdk (streamed to disk; staged
 *                             for the next start while Sonic Mania's pack is in use)
 *   GET  /api/files           JSON: saves/settings files, Sonic Mania's and each retro game's
 *   GET  /api/files/:name[?game=<id>]   download one file (a retro game's with ?game=sonic1 ...)
 *   PUT  /api/files/:name[?game=<id>]   raw body = replacement file (applied at next launch while playing)
 *   PUT  /api/settings?plusEnabled=0|1&retroEnabled=0|1&maniaAudio=0|1&maniaSimilarMusic=0|1&maniaRemixes=0|1
 *                             (any of them)
 *   GET  /api/retro           JSON: retro mode, TV and USB space, each game's packs
 *   PUT  /api/retro/:game/:edition?dest=tv|usb[&mount=<USB mount point>]
 *                             raw body = a retro game's pack (edition: origins|classic)
 *   DELETE /api/retro/:game/:edition   removes that pack wherever it is
 *   POST /api/retro/usb-folder?mount=<USB mount point>   creates SonicWebMania/retro on the drive
 *   POST /api/move?pack=mania|<game>-<edition>&dest=tv|usb[&mount=<USB mount point>]
 *                             moves a pack between the TV and a USB drive (one in use is
 *                             copied now and switched at the next start)
 *   GET  /api/cleanup         JSON: leftovers of interrupted uploads, moves and conversions
 *   POST /api/cleanup         removes them
 *
 * Packs the running game reads from (TransferServer::SetPacksInUse) are never
 * moved, replaced or removed: the engine reopens them by path for every file.
 */
#include "TransferServer.h"

#include "NetInfo.h"
#include "PortInfo.h"
#include "PortConfig.h"
#include "RetroGames.h"
#include "RsdkPack.h"
#include "Storage.h"
#include "UsbDrives.h"
#include "WebPage.generated.h"

// Keep the server light: two workers are plenty for one person with a phone.
#define CPPHTTPLIB_THREAD_POOL_COUNT 2
#include <httplib.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <filesystem>
#include <system_error>
#include <vector>

namespace swm
{
namespace
{

constexpr const char *kJsonType   = "application/json";
constexpr const char *kTextType   = "text/plain; charset=utf-8";
constexpr const char *kPinHeader  = "X-Pin";
constexpr int kPortAttempts       = 10;
constexpr std::uint64_t kMinimumPackBytes = 64; // Header plus a file table; anything smaller is not a pack.
/** Space kept free on a USB drive on top of a pack (the TV keeps Storage::kSafetyMarginBytes). */
constexpr std::uint64_t kUsbSafetyMarginBytes = 8ull * 1024ull * 1024ull;
/** Largest retro pack accepted (the biggest known, Sonic CD's, is far below this). */
constexpr std::uint64_t kMaxRetroPackBytes = 1024ull * 1024ull * 1024ull;

/** Escapes @p text for use inside a JSON string literal. */
std::string JsonEscape(const std::string &text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

/** Formats a byte count for messages shown to people ("1.5 GB"). */
std::string HumanBytes(std::uint64_t bytes)
{
    constexpr const char *kUnits[] = { "B", "KB", "MB", "GB" };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

/** Sends a plain-text error. */
void Fail(httplib::Response &res, int status, const std::string &message)
{
    res.status = status;
    res.set_content(message, kTextType);
}

/**
 * Reads the announced body size. Uploads must say how big they are so free
 * space can be checked before writing.
 *
 * @return False if the header is missing or malformed.
 */
bool ContentLength(const httplib::Request &req, std::uint64_t *out)
{
    const std::string value = req.get_header_value("Content-Length");
    if (value.empty())
        return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(value, &consumed);
        if (consumed != value.size())
            return false;
        *out = static_cast<std::uint64_t>(parsed);
        return true;
    }
    catch (const std::exception &) {
        return false;
    }
}

/**
 * Streams the request body into @p path, refusing to write more than
 * @p expected bytes. The file is flushed to storage before returning.
 *
 * @param onProgress Called with the running total after every chunk.
 * @return Number of bytes written, or nullopt-style -1 on I/O error.
 */
template <typename ProgressFn>
std::int64_t StreamBodyToFile(const httplib::ContentReader &reader, const std::string &path, std::uint64_t expected,
                              ProgressFn onProgress)
{
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out)
        return -1;

    std::uint64_t written = 0;
    const bool completed  = reader([&](const char *data, std::size_t length) {
        if (written + length > expected)
            return false; // Client sent more than it announced.
        if (std::fwrite(data, 1, length, out) != length)
            return false; // Disk full or I/O error.
        written += length;
        onProgress(written);
        return true;
    });

    const bool flushed = std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);

    if (!completed || !flushed)
        return -1;
    return static_cast<std::int64_t>(written);
}

/**
 * The folder (relative to the data directory) a /api/files request is about:
 * "" for Sonic Mania's files, "games/<id>" with ?game=<id> for a retro game.
 *
 * @return False for an unknown game.
 */
bool FilesFolder(const httplib::Request &req, std::string *folder)
{
    const std::string game = req.get_param_value("game");
    if (game.empty()) {
        folder->clear();
        return true;
    }
    if (!RetroLibrary::FindGame(game))
        return false;
    *folder = std::string(Storage::kGamesFolderName) + "/" + game;
    return true;
}

/** Canonical form of @p path, to compare paths that may be spelled differently. */
std::string CanonicalPath(const std::string &path)
{
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path : canonical.string();
}

/** Where an upload or a move is going: the TV, or a USB drive. */
struct Destination {
    bool usb = false;
    UsbDrive drive;          ///< The drive, when @c usb.
    std::uint64_t free   = 0; ///< Bytes free there.
    std::uint64_t margin = 0; ///< Bytes to keep free there on top of the file.
};

/**
 * Reads ?dest=tv|usb&mount=... (the TV when there is no dest).
 * @return False with an HTTP error already set when the drive is unusable.
 */
bool ParseDestination(const httplib::Request &req, const Storage &storage, Destination *dest, httplib::Response &res)
{
    if (req.get_param_value("dest") != "usb") {
        dest->usb    = false;
        dest->free   = storage.FreeBytes();
        dest->margin = Storage::kSafetyMarginBytes;
        return true;
    }
    if (!FindUsbDrive(req.get_param_value("mount"), &dest->drive)) {
        Fail(res, 404, "That USB drive is not connected.");
        return false;
    }
    if (!dest->drive.writable) {
        Fail(res, 403, "That USB drive is read-only.");
        return false;
    }
    dest->usb    = true;
    dest->free   = dest->drive.freeBytes;
    dest->margin = kUsbSafetyMarginBytes;
    return true;
}

/** The USB drive @p path is on, if any. */
bool DriveOf(const std::string &path, UsbDrive *drive)
{
    for (const UsbDrive &candidate : ListUsbDrives()) {
        if (path.rfind(candidate.mountPoint + "/", 0) == 0) {
            *drive = candidate;
            return true;
        }
    }
    return false;
}

/**
 * Copies @p from to @p to in chunks, flushing it to storage, reporting the
 * running total to @p onProgress. @return False on any read or write error.
 */
template <typename ProgressFn>
bool CopyFileWithProgress(const std::string &from, const std::string &to, ProgressFn onProgress)
{
    std::FILE *in = std::fopen(from.c_str(), "rb");
    if (!in)
        return false;
    std::FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }

    std::vector<char> buffer(1024 * 1024);
    std::uint64_t total = 0;
    bool ok             = true;
    for (;;) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), in);
        if (read == 0) {
            ok = !std::ferror(in);
            break;
        }
        if (std::fwrite(buffer.data(), 1, read, out) != read) {
            ok = false;
            break;
        }
        total += read;
        onProgress(total);
    }
    ok = ok && std::fflush(out) == 0 && fsync(fileno(out)) == 0;
    std::fclose(out);
    std::fclose(in);
    return ok;
}

/** Every place Sonic Mania's pack may be: the TV's data folder, then each USB drive. */
std::vector<std::string> ManiaPackPaths(const Storage &storage)
{
    std::vector<std::string> paths = { storage.PathFor(Storage::kDataPackName) };
    for (const UsbDrive &drive : ListUsbDrives())
        paths.push_back(Storage::UsbDataPackPath(drive.mountPoint));
    return paths;
}

/** Every place a retro pack may be: the TV's pack folder, then each USB drive's. */
std::vector<std::string> PackPaths(const RetroLibrary &retro, const std::string &gameId, RetroEdition edition)
{
    std::vector<std::string> paths;
    for (const std::string &folder : retro.PackFolders())
        paths.push_back(folder + "/" + RetroLibrary::PackFileName(gameId, edition));
    return paths;
}

/**
 * Where a staged copy of a pack waits (`<path>.pending` for one of @p paths,
 * the TV's first): "tv", "usb", or "" when nothing is staged.
 */
std::string PendingLocation(const std::vector<std::string> &paths)
{
    for (std::size_t i = 0; i < paths.size(); ++i) {
        std::error_code ec;
        if (std::filesystem::exists(Storage::PendingPathFor(paths[i]), ec))
            return i == 0 ? "tv" : "usb";
    }
    return {};
}

/** Suffixes of files an interrupted upload, move or Sonic CD conversion leaves behind. */
constexpr const char *kLeftoverSuffixes[] = { ".part", ".conv" };

/**
 * Files the app can safely delete: leftovers of interrupted transfers (see
 * kLeftoverSuffixes) in every folder it writes to, on the TV and the USB
 * drives, and the engine's log files (only written by builds with
 * SWM_ENGINE_LOG on). Staged files (.pending, incoming/) are not included:
 * they are applied at the next start.
 */
std::vector<std::string> CleanableFiles(const Storage &storage, const RetroLibrary &retro)
{
    namespace fs = std::filesystem;
    std::vector<std::string> folders = { storage.Root(), storage.IncomingDir() };
    std::error_code ec;
    for (const char *parent : { Storage::kGamesFolderName, "incoming/games" }) {
        for (const fs::directory_entry &game : fs::directory_iterator(storage.PathFor(parent), ec))
            folders.push_back(game.path().string());
    }
    for (const std::string &packFolder : retro.PackFolders())
        folders.push_back(packFolder);
    for (const UsbDrive &drive : ListUsbDrives())
        folders.push_back(Storage::UsbFolder(drive.mountPoint));

    std::vector<std::string> files;
    for (const std::string &folder : folders) {
        for (const fs::directory_entry &entry : fs::directory_iterator(folder, ec)) {
            std::error_code typeError;
            if (!entry.is_regular_file(typeError))
                continue;
            const std::string name = entry.path().filename().string();
            bool cleanable         = name == "log.txt";
            for (const char *suffix : kLeftoverSuffixes) {
                const std::size_t length = std::strlen(suffix);
                cleanable = cleanable || (name.size() > length && name.compare(name.size() - length, length, suffix) == 0);
            }
            if (cleanable)
                files.push_back(entry.path().string());
        }
    }
    return files;
}

/** JSON array of @p files. */
std::string FileListJson(const std::vector<UserFile> &files)
{
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const UserFile &file : files) {
        json << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(file.name) << "\",\"size\":" << file.size
             << ",\"mtime\":" << file.mtime << "}";
        first = false;
    }
    json << "]";
    return json.str();
}

} // namespace

std::string GeneratePin()
{
    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 9999);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", digits(device));
    return buffer;
}

TransferServer::TransferServer(Storage &storage, PortConfig &config, const RetroLibrary &retro, std::string pin)
    : storage_(storage), config_(config), retro_(retro), pin_(std::move(pin))
{
}

TransferServer::~TransferServer() { Stop(); }

bool TransferServer::Start()
{
    if (server_)
        return port_.load() != 0;

    server_ = std::make_unique<httplib::Server>();
    server_->new_task_queue = [] { return new httplib::ThreadPool(CPPHTTPLIB_THREAD_POOL_COUNT); };
    server_->set_payload_max_length(std::numeric_limits<std::size_t>::max());
    server_->set_read_timeout(30, 0);
    server_->set_write_timeout(30, 0);
    // One request per connection: with two workers, browsers' idle keep-alive
    // connections would otherwise hold both and stall the page's next request.
    server_->set_keep_alive_max_count(1);
    RegisterRoutes();

    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const int port = kPreferredPort + attempt;
        if (server_->bind_to_port("0.0.0.0", port)) {
            port_.store(static_cast<std::uint16_t>(port));
            thread_ = std::thread([this] { server_->listen_after_bind(); });
            return true;
        }
    }

    server_.reset();
    return false;
}

void TransferServer::Stop()
{
    if (!server_)
        return;

    server_->stop();
    if (thread_.joinable())
        thread_.join();
    server_.reset();
    port_.store(0);
}

TransferStatus TransferServer::Snapshot() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void TransferServer::PublishEvent(const std::string &message)
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.lastEvent = message;
    ++status_.eventSerial;
}

void TransferServer::NoteRestartNeeded()
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_.restartPending = status_.restartPending || !setupMode_.load();
}

void TransferServer::SetPacksInUse(const std::vector<std::string> &paths)
{
    std::vector<std::string> canonical;
    for (const std::string &path : paths) {
        if (!path.empty())
            canonical.push_back(CanonicalPath(path));
    }
    std::lock_guard<std::mutex> lock(packsMutex_);
    packsInUse_ = std::move(canonical);
}

bool TransferServer::IsPackInUse(const std::string &path) const
{
    const std::string canonical = CanonicalPath(path);
    std::lock_guard<std::mutex> lock(packsMutex_);
    return std::find(packsInUse_.begin(), packsInUse_.end(), canonical) != packsInUse_.end();
}

void TransferServer::RegisterRoutes()
{
    httplib::Server &server = *server_;

    // Every request: private networks only, and /api needs the on-screen PIN.
    server.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        if (!IsPrivateNetworkAddress(req.remote_addr)) {
            Fail(res, 403, "Only devices on the local network may connect.");
            return httplib::Server::HandlerResponse::Handled;
        }
        // The PIN comes in a header for API calls, or as ?pin= for plain download links.
        const std::string pin = req.has_header(kPinHeader) ? req.get_header_value(kPinHeader) : req.get_param_value("pin");
        if (req.path.rfind("/api/", 0) == 0 && pin != pin_) {
            Fail(res, 401, "Wrong or missing PIN. Use the PIN shown on the TV.");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Cache-Control", "no-store");
        res.set_content(kWebPageHtml, "text/html; charset=utf-8");
    });

    server.Get("/api/status", [this](const httplib::Request &, httplib::Response &res) {
        const TransferStatus status = Snapshot();
        const std::string packPath  = storage_.DataPackPath();
        const bool packValid        = !packPath.empty();
        UsbDrive packDrive;
        const bool packOnUsb = packValid && DriveOf(packPath, &packDrive);

        std::ostringstream json;
        json << "{\"version\":\"" << kAppVersion << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"persistent\":" << (storage_.IsPersistent() ? "true" : "false")
             << ",\"dataPack\":{\"present\":" << (packValid ? "true" : "false")
             << ",\"size\":" << (packValid ? Storage::FileSize(packPath) : 0) << ",\"location\":\""
             << (packOnUsb ? "usb" : "tv") << "\",\"mount\":\"" << JsonEscape(packOnUsb ? packDrive.mountPoint : "")
             << "\",\"inUse\":" << (packValid && IsPackInUse(packPath) ? "true" : "false") << ",\"pending\":\""
             << PendingLocation(ManiaPackPaths(storage_)) << "\"}"
             << ",\"plusEnabled\":" << (config_.IsPlusEnabled() ? "true" : "false")
             << ",\"retroEnabled\":" << (config_.IsRetroEnabled() ? "true" : "false")
             << ",\"maniaAudio\":" << (config_.IsManiaAudioEnabled() ? "true" : "false")
             << ",\"maniaSimilarMusic\":" << (config_.IsManiaSimilarMusicEnabled() ? "true" : "false")
             << ",\"maniaRemixes\":" << (config_.IsManiaRemixesEnabled() ? "true" : "false")
             << ",\"restartPending\":" << (status.restartPending ? "true" : "false")
             << ",\"upload\":{\"active\":" << (status.uploading ? "true" : "false")
             << ",\"received\":" << status.receivedBytes << ",\"expected\":" << status.expectedBytes << "}}";
        res.set_content(json.str(), kJsonType);
    });

    server.Put("/api/data", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected < kMinimumPackBytes)
            return Fail(res, 411, "The upload must state its size (Content-Length).");

        // Where it goes: the data folder on the TV, or SonicWebMania/ on a USB drive.
        Destination dest;
        if (!ParseDestination(req, storage_, &dest, res))
            return;
        const std::string folder = dest.usb ? Storage::UsbFolder(dest.drive.mountPoint) : storage_.Root();
        const std::string target = dest.usb ? Storage::UsbDataPackPath(dest.drive.mountPoint) : storage_.PathFor(Storage::kDataPackName);
        const std::uint64_t needed = expected + dest.margin;
        if (needed > dest.free) {
            return Fail(res, 507,
                        std::string("Not enough space on the ") + (dest.usb ? "USB drive" : "TV") + ": this file needs "
                            + HumanBytes(needed) + " (including a safety margin), " + HumanBytes(dest.free) + " is free.");
        }

        if (uploadInProgress_.exchange(true))
            return Fail(res, 409, "Another upload is already running.");
        // Clear the flag however this handler exits.
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &uploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec)
            return Fail(res, 500, "Could not create " + folder + ": " + ec.message());

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = expected;
        }
        PublishEvent("Receiving game data...");

        const std::string partPath = target + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }

        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            std::filesystem::remove(partPath, ec);
            PublishEvent("Upload interrupted. Please try again.");
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }
        if (!Storage::IsValidDataPackHeader(partPath)) {
            std::filesystem::remove(partPath, ec);
            PublishEvent("That file is not a Data.rsdk.");
            return Fail(res, 415, "That file is not a Sonic Mania Data.rsdk (missing RSDK signature).");
        }

        // While the engine reads Sonic Mania's pack (playing it, or a retro
        // game borrowing its audio), the new one waits for the next start;
        // Storage::ApplyPendingFiles() installs it and removes other copies.
        const std::string current = storage_.DataPackPath();
        const bool stage          = !current.empty() && IsPackInUse(current);
        const std::string destination = stage ? Storage::PendingPathFor(target) : target;
        std::filesystem::rename(partPath, destination, ec);
        if (ec) {
            std::filesystem::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }
        if (!stage) {
            // One copy only: the TV's is used first, so another would shadow or confuse.
            std::vector<std::string> copies = { storage_.PathFor(Storage::kDataPackName) };
            for (const UsbDrive &drive : ListUsbDrives())
                copies.push_back(Storage::UsbDataPackPath(drive.mountPoint));
            for (const std::string &copy : copies) {
                if (CanonicalPath(copy) != CanonicalPath(target))
                    std::filesystem::remove(copy, ec);
            }
        }

        const bool setup = setupMode_.load();
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            if (setup)
                status_.dataPackReady = true;
            else
                status_.restartPending = true;
        }
        PublishEvent(setup ? "Game data installed." : stage ? "New game data received. Restart to use it." : "Game data installed.");
        res.set_content(setup ? "installed" : stage ? "staged" : "stored", kTextType);
    });

    server.Get("/api/files", [this](const httplib::Request &, httplib::Response &res) {
        std::ostringstream json;
        json << "{\"files\":" << FileListJson(storage_.ListUserFiles()) << ",\"games\":[";
        // Every retro game, also before its first start (saves can be uploaded ahead).
        bool first = true;
        for (const RetroGameInfo &game : RetroCatalogue()) {
            const std::string folder = std::string(Storage::kGamesFolderName) + "/" + game.id;
            json << (first ? "" : ",") << "{\"id\":\"" << game.id << "\",\"title\":\"" << JsonEscape(game.title)
                 << "\",\"files\":" << FileListJson(storage_.ListUserFiles(folder)) << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/files/:name", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedUserFileName(name))
            return Fail(res, 400, "That file name is not allowed.");
        std::string folder;
        if (!FilesFolder(req, &folder))
            return Fail(res, 404, "Unknown game.");

        const std::string path = storage_.PathFor(folder.empty() ? name : folder + "/" + name);
        const std::uint64_t size = Storage::FileSize(path);
        if (size == 0 || size > Storage::kMaxUserFileBytes)
            return Fail(res, 404, "File not found.");

        std::ifstream in(path, std::ios::binary);
        std::string body(static_cast<std::size_t>(size), '\0');
        if (!in.read(&body[0], static_cast<std::streamsize>(size)))
            return Fail(res, 500, "Could not read the file.");

        // A retro game's file is named after it ("sonic1-SGame.bin"), so downloads do not overwrite each other.
        const std::string downloadName = folder.empty() ? name : req.get_param_value("game") + "-" + name;
        res.set_header("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
        res.set_content(std::move(body), "application/octet-stream");
    });

    server.Put("/api/files/:name", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedUserFileName(name))
            return Fail(res, 400, "Only .bin save files and .ini settings files can be uploaded.");
        std::string folder;
        if (!FilesFolder(req, &folder))
            return Fail(res, 404, "Unknown game.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxUserFileBytes)
            return Fail(res, 413, "Save files are never this big; refusing " + HumanBytes(expected) + ".");
        if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return Fail(res, 507, "Not enough space on the TV.");

        // While playing, the engine owns its save files: stage the upload for the next launch.
        const bool setup             = setupMode_.load();
        const std::string targetDir  = folder.empty() ? storage_.Root() : storage_.PathFor(folder);
        const std::string stagingDir = folder.empty() ? storage_.IncomingDir() : storage_.IncomingDir() + "/" + folder;
        std::error_code ec;
        std::filesystem::create_directories(setup ? targetDir : stagingDir, ec);
        const std::string destination = (setup ? targetDir : stagingDir) + "/" + name;
        const std::string partPath    = destination + ".part";

        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [](std::uint64_t) {});
        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            std::filesystem::remove(partPath, ec);
            return Fail(res, 400, "The upload was interrupted.");
        }
        std::filesystem::rename(partPath, destination, ec);
        if (ec) {
            std::filesystem::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }

        if (!setup) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.restartPending = true;
        }
        const std::string label = folder.empty() ? name : std::string(RetroLibrary::FindGame(req.get_param_value("game"))->title) + ": " + name;
        PublishEvent(setup ? label + " uploaded." : label + " received. Restart to use it.");
        res.set_content(setup ? "installed" : "staged", kTextType);
    });

    server.Put("/api/settings", [this](const httplib::Request &req, httplib::Response &res) {
        if (!req.has_param("plusEnabled") && !req.has_param("retroEnabled") && !req.has_param("maniaAudio")
            && !req.has_param("maniaSimilarMusic") && !req.has_param("maniaRemixes"))
            return Fail(res, 400, "Nothing to change.");

        std::string message;
        if (req.has_param("plusEnabled")) {
            const bool enabled = req.get_param_value("plusEnabled") == "1";
            if (!config_.SetPlusEnabled(enabled))
                return Fail(res, 500, "Could not save the setting.");
            message = enabled ? "Plus content enabled. Restart to apply." : "Plus content disabled. Restart to apply.";
        }
        if (req.has_param("retroEnabled")) {
            const bool enabled = req.get_param_value("retroEnabled") == "1";
            if (!config_.SetRetroEnabled(enabled))
                return Fail(res, 500, "Could not save the setting.");
            message = enabled ? "Retro games enabled. Restart to apply." : "Retro games disabled. Restart to apply.";
        }
        if (req.has_param("maniaAudio")) {
            const bool enabled = req.get_param_value("maniaAudio") == "1";
            if (!config_.SetManiaAudioEnabled(enabled))
                return Fail(res, 500, "Could not save the setting.");
            message = enabled ? "Sonic Mania's audio fills retro gaps. Restart to apply."
                              : "Sonic Mania's audio no longer used in retro games. Restart to apply.";
        }
        if (req.has_param("maniaSimilarMusic")) {
            const bool enabled = req.get_param_value("maniaSimilarMusic") == "1";
            if (!config_.SetManiaSimilarMusicEnabled(enabled))
                return Fail(res, 500, "Could not save the setting.");
            message = enabled ? "Similar Mania music on. Restart to apply." : "Similar Mania music off. Restart to apply.";
        }
        if (req.has_param("maniaRemixes")) {
            const bool enabled = req.get_param_value("maniaRemixes") == "1";
            if (!config_.SetManiaRemixesEnabled(enabled))
                return Fail(res, 500, "Could not save the setting.");
            message = enabled ? "Sonic Mania remixes on. Restart to apply." : "Sonic Mania remixes off. Restart to apply.";
        }

        NoteRestartNeeded();
        PublishEvent(message);
        res.set_content("saved", kTextType);
    });

    RegisterRetroRoutes();
}

namespace
{

/**
 * JSON for one pack slot of a retro game (@p inUse: the running game reads
 * it; @p pending: where a staged replacement waits, "" for none).
 */
std::string PackJson(const RetroPack &pack, bool inUse, const std::string &pending)
{
    std::ostringstream json;
    json << "{\"state\":\"" << PackStateName(pack.state) << "\",\"pending\":\"" << pending << "\"";
    if (pack.state != PackState::Missing) {
        json << ",\"location\":\"" << (pack.onUsb ? "usb" : "tv") << "\",\"path\":\"" << JsonEscape(pack.path)
             << "\",\"size\":" << pack.size;
        if (pack.version)
            json << ",\"version\":\"" << pack.version << "\"";
        json << ",\"standalone\":" << (pack.standalone ? "true" : "false") << ",\"inUse\":" << (inUse ? "true" : "false");
    }
    json << "}";
    return json.str();
}

/** Parses the :edition path part. @return False for anything but "origins" or "classic". */
bool ParseEdition(const std::string &text, RetroEdition *edition)
{
    if (text == "origins")
        *edition = RetroEdition::Origins;
    else if (text == "classic")
        *edition = RetroEdition::Classic;
    else
        return false;
    return true;
}

/** "Sonic CD (Origins)" for messages. */
std::string PackLabel(const RetroGameInfo &game, RetroEdition edition)
{
    return std::string(game.title) + (edition == RetroEdition::Origins ? " (Origins)" : " (classic)");
}


} // namespace

void TransferServer::RegisterRetroRoutes()
{
    httplib::Server &server = *server_;

    server.Get("/api/retro", [this](const httplib::Request &, httplib::Response &res) {
        std::ostringstream json;
        json << "{\"enabled\":" << (config_.IsRetroEnabled() ? "true" : "false") << ",\"tv\":{\"folder\":\""
             << JsonEscape(retro_.TvPackFolder()) << "\",\"freeBytes\":" << storage_.FreeBytes()
             << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes << "},\"drives\":[";
        bool first = true;
        for (const UsbDrive &drive : ListUsbDrives()) {
            std::error_code ec;
            const std::string folder = RetroLibrary::UsbPackFolder(drive.mountPoint);
            json << (first ? "" : ",") << "{\"mount\":\"" << JsonEscape(drive.mountPoint) << "\",\"name\":\""
                 << JsonEscape(drive.name) << "\",\"fileSystem\":\"" << JsonEscape(drive.fileSystem)
                 << "\",\"freeBytes\":" << drive.freeBytes << ",\"totalBytes\":" << drive.totalBytes
                 << ",\"writable\":" << (drive.writable ? "true" : "false")
                 << ",\"hasFolder\":" << (std::filesystem::is_directory(folder, ec) ? "true" : "false") << ",\"folder\":\""
                 << JsonEscape(folder) << "\"}";
            first = false;
        }
        json << "],\"games\":[";
        first = true;
        for (const RetroGame &game : retro_.Scan()) {
            json << (first ? "" : ",") << "{\"id\":\"" << game.info.id << "\",\"title\":\"" << JsonEscape(game.info.title)
                 << "\",\"playable\":" << (game.Playable() ? "true" : "false")
                 << ",\"sound\":" << (game.HasSound() ? "true" : "false") << ",\"mode\":\""
                 << JsonEscape(game.ModeDescription()) << "\",\"origins\":"
                 << PackJson(game.origins, game.origins.state != PackState::Missing && IsPackInUse(game.origins.path),
                             PendingLocation(PackPaths(retro_, game.info.id, RetroEdition::Origins)))
                 << ",\"classic\":"
                 << PackJson(game.classic, game.classic.state != PackState::Missing && IsPackInUse(game.classic.path),
                             PendingLocation(PackPaths(retro_, game.info.id, RetroEdition::Classic)))
                 << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Post("/api/retro/usb-folder", [](const httplib::Request &req, httplib::Response &res) {
        UsbDrive drive;
        if (!FindUsbDrive(req.get_param_value("mount"), &drive))
            return Fail(res, 404, "That USB drive is not connected.");
        if (!drive.writable)
            return Fail(res, 403, "That USB drive is read-only.");

        std::error_code ec;
        const std::string folder = RetroLibrary::UsbPackFolder(drive.mountPoint);
        std::filesystem::create_directories(folder, ec);
        if (ec)
            return Fail(res, 500, "Could not create " + folder + ": " + ec.message());
        res.set_content("created", kTextType);
    });

    server.Put("/api/retro/:game/:edition",
               [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const RetroGameInfo *game = RetroLibrary::FindGame(req.path_params.at("game"));
        RetroEdition edition      = RetroEdition::Origins;
        if (!game || !ParseEdition(req.path_params.at("edition"), &edition))
            return Fail(res, 404, "Unknown game or edition.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected < kMinimumPackBytes)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > kMaxRetroPackBytes)
            return Fail(res, 413, "No retro game pack is this big; refusing " + HumanBytes(expected) + ".");

        // Where it goes: the TV's data folder, or a USB drive's SonicWebMania/retro folder.
        std::string folder;
        std::uint64_t free = 0, margin = 0;
        if (req.get_param_value("dest") == "usb") {
            UsbDrive drive;
            if (!FindUsbDrive(req.get_param_value("mount"), &drive))
                return Fail(res, 404, "That USB drive is not connected.");
            if (!drive.writable)
                return Fail(res, 403, "That USB drive is read-only.");
            folder = RetroLibrary::UsbPackFolder(drive.mountPoint);
            free   = drive.freeBytes;
            margin = kUsbSafetyMarginBytes;
        }
        else {
            folder = retro_.TvPackFolder();
            free   = storage_.FreeBytes();
            margin = Storage::kSafetyMarginBytes;
        }
        if (expected + margin > free) {
            return Fail(res, 507,
                        "Not enough space: this file needs " + HumanBytes(expected + margin) + " (including a safety margin), "
                            + HumanBytes(free) + " is free.");
        }

        // Installing replaces this pack wherever it is. While the game reads
        // it, the new one waits as <pack>.pending for the next start
        // (RetroLibrary::ApplyPendingPacks).
        const std::string label = PackLabel(*game, edition);
        bool stage              = false;
        for (const std::string &other : retro_.PackFolders())
            stage = stage || IsPackInUse(other + "/" + RetroLibrary::PackFileName(game->id, edition));

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec)
            return Fail(res, 500, "Could not create " + folder + ": " + ec.message());

        if (uploadInProgress_.exchange(true))
            return Fail(res, 409, "Another upload is already running.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &uploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = expected;
        }
        PublishEvent("Receiving " + label + "...");

        const std::string fileName = RetroLibrary::PackFileName(game->id, edition);
        const std::string partPath = folder + "/" + fileName + ".part";
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }

        if (written < 0 || static_cast<std::uint64_t>(written) != expected) {
            std::filesystem::remove(partPath, ec);
            PublishEvent("Upload interrupted. Please try again.");
            return Fail(res, 400, "The upload was interrupted or could not be written.");
        }

        char version    = 0;
        PackState state = RetroLibrary::Inspect(partPath, &version);
        std::string readyPath = partPath; // The file to install.
        if (state == PackState::Unsupported) {
            // Sonic CD's 2011 format: rewrite it as a pack the engine opens.
            // The converted copy is written next to the upload before the upload is removed.
            std::uint64_t freeAfter = 0;
            {
                UsbDrive drive;
                freeAfter = req.get_param_value("dest") == "usb" && FindUsbDrive(req.get_param_value("mount"), &drive)
                                ? drive.freeBytes
                                : storage_.FreeBytes();
            }
            if (expected + margin > freeAfter) {
                std::filesystem::remove(partPath, ec);
                return Fail(res, 507, "Not enough space to convert this pack: it needs another " + HumanBytes(expected + margin) + ".");
            }
            PublishEvent("Converting " + label + "...");
            const std::string convertedPath = folder + "/" + fileName + ".conv";
            std::string error;
            const bool converted = RsdkPack::ConvertOldV3Pack(partPath, convertedPath, &error);
            std::filesystem::remove(partPath, ec);
            if (!converted) {
                PublishEvent("Could not convert " + label + ".");
                return Fail(res, 415, "Could not convert this Sonic CD pack: " + error + ".");
            }
            readyPath = convertedPath;
            state     = RetroLibrary::Inspect(readyPath, &version);
        }
        if (state != PackState::Ready) {
            std::filesystem::remove(readyPath, ec);
            PublishEvent("That file is not a " + label + " pack.");
            return Fail(res, 415, "That file is not an RSDKv3/v4 data pack (a Sonic Mania Data.rsdk belongs above).");
        }

        const std::string target      = folder + "/" + fileName;
        const std::string destination = stage ? Storage::PendingPathFor(target) : target;
        std::filesystem::rename(readyPath, destination, ec);
        if (ec) {
            std::filesystem::remove(readyPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }

        // One copy only: the TV's folder is searched first, so an older copy there would win.
        if (!stage) {
            for (const std::string &other : retro_.PackFolders()) {
                if (other != folder)
                    std::filesystem::remove(other + "/" + fileName, ec);
            }
        }

        NoteRestartNeeded();
        PublishEvent(stage ? label + " received. It replaces the one in use at the next start." : label + " installed.");
        res.set_content(stage ? "staged" : "installed", kTextType);
    });

    server.Delete("/api/retro/:game/:edition", [this](const httplib::Request &req, httplib::Response &res) {
        const RetroGameInfo *game = RetroLibrary::FindGame(req.path_params.at("game"));
        RetroEdition edition      = RetroEdition::Origins;
        if (!game || !ParseEdition(req.path_params.at("edition"), &edition))
            return Fail(res, 404, "Unknown game or edition.");

        const std::string fileName = RetroLibrary::PackFileName(game->id, edition);
        for (const std::string &folder : retro_.PackFolders()) {
            if (IsPackInUse(folder + "/" + fileName))
                return Fail(res, 409, PackLabel(*game, edition) + " is in use by the game running now. Switch to another game first.");
        }
        int removed = 0;
        for (const std::string &folder : retro_.PackFolders()) {
            std::error_code ec;
            if (std::filesystem::remove(folder + "/" + fileName, ec))
                ++removed;
        }
        if (removed == 0)
            return Fail(res, 404, "That pack is not installed.");

        NoteRestartNeeded();
        PublishEvent(PackLabel(*game, edition) + " removed.");
        res.set_content("removed", kTextType);
    });

    server.Post("/api/move", [this](const httplib::Request &req, httplib::Response &res) {
        // Which pack: "mania", or "<game>-<edition>" (sonic1-origins...).
        const std::string pack = req.get_param_value("pack");
        std::string source, label, fileName;
        bool retroPack = false;
        if (pack == "mania") {
            source   = storage_.DataPackPath();
            label    = "Sonic Mania's Data.rsdk";
            fileName = Storage::kDataPackName;
        }
        else {
            const std::size_t dash = pack.rfind('-');
            RetroEdition edition   = RetroEdition::Origins;
            const RetroGameInfo *game = dash == std::string::npos ? nullptr : RetroLibrary::FindGame(pack.substr(0, dash));
            if (!game || !ParseEdition(pack.substr(dash + 1), &edition))
                return Fail(res, 404, "Unknown pack.");
            for (const RetroGame &found : retro_.Scan()) {
                if (found.info.id == std::string(game->id)) {
                    const RetroPack &slot = edition == RetroEdition::Origins ? found.origins : found.classic;
                    if (slot.state != PackState::Missing)
                        source = slot.path;
                }
            }
            label     = PackLabel(*game, edition);
            fileName  = RetroLibrary::PackFileName(game->id, edition);
            retroPack = true;
        }
        if (source.empty())
            return Fail(res, 404, label + " is not installed.");
        // A pack the game reads is copied now and switched at the next start
        // (Storage::ApplyPendingFiles / RetroLibrary::ApplyPendingPacks remove
        // the original then); any other moves at once.
        const bool stage = IsPackInUse(source);

        Destination dest;
        if (!ParseDestination(req, storage_, &dest, res))
            return;
        const std::string folder = retroPack ? (dest.usb ? RetroLibrary::UsbPackFolder(dest.drive.mountPoint) : retro_.TvPackFolder())
                                             : (dest.usb ? Storage::UsbFolder(dest.drive.mountPoint) : storage_.Root());
        const std::string target = folder + "/" + fileName;
        if (CanonicalPath(target) == CanonicalPath(source))
            return Fail(res, 400, label + " is already there.");

        const std::uint64_t size = Storage::FileSize(source);
        if (size + dest.margin > dest.free) {
            return Fail(res, 507,
                        "Not enough space there: " + label + " needs " + HumanBytes(size + dest.margin)
                            + " (including a safety margin), " + HumanBytes(dest.free) + " is free.");
        }

        if (uploadInProgress_.exchange(true))
            return Fail(res, 409, "Another upload or move is already running.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &uploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec)
            return Fail(res, 500, "Could not create " + folder + ": " + ec.message());

        // Copy, then remove the original: the TV and a drive are different file systems.
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = size;
        }
        PublishEvent(std::string("Moving ") + label + " to the " + (dest.usb ? "USB drive" : "TV") + "...");
        const std::string partPath = target + ".part";
        const bool copied          = CopyFileWithProgress(source, partPath, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });
        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }
        if (!copied || Storage::FileSize(partPath) != size) {
            std::filesystem::remove(partPath, ec);
            PublishEvent("Could not move " + label + ".");
            return Fail(res, 500, "Could not copy " + label + " there (the original is untouched).");
        }
        std::filesystem::rename(partPath, stage ? Storage::PendingPathFor(target) : target, ec);
        if (ec) {
            std::filesystem::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place (the original is untouched).");
        }
        if (stage) {
            NoteRestartNeeded();
            PublishEvent(label + " copied. The move finishes at the next start.");
            res.set_content("staged", kTextType);
            return;
        }
        std::filesystem::remove(source, ec);
        if (ec) {
            // Keep exactly one copy: the original stays, the new one goes.
            std::filesystem::remove(target, ec);
            return Fail(res, 500, "Could not remove the original " + label + ", so it was left where it was.");
        }

        PublishEvent(label + std::string(" moved to the ") + (dest.usb ? "USB drive." : "TV."));
        res.set_content("moved", kTextType);
    });

    server.Get("/api/cleanup", [this](const httplib::Request &, httplib::Response &res) {
        std::uint64_t bytes = 0;
        const std::vector<std::string> files = CleanableFiles(storage_, retro_);
        for (const std::string &file : files)
            bytes += Storage::FileSize(file);
        std::ostringstream json;
        json << "{\"files\":" << files.size() << ",\"bytes\":" << bytes
             << ",\"busy\":" << (uploadInProgress_.load() ? "true" : "false") << "}";
        res.set_content(json.str(), kJsonType);
    });

    server.Post("/api/cleanup", [this](const httplib::Request &, httplib::Response &res) {
        // A running upload or move writes one of these files: wait for it.
        if (uploadInProgress_.exchange(true))
            return Fail(res, 409, "An upload or move is running. Clean up when it has finished.");
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &uploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        std::uint64_t freed = 0;
        int removed         = 0;
        for (const std::string &file : CleanableFiles(storage_, retro_)) {
            const std::uint64_t size = Storage::FileSize(file);
            std::error_code ec;
            if (std::filesystem::remove(file, ec)) {
                freed += size;
                ++removed;
            }
        }
        PublishEvent("Cleaned up " + HumanBytes(freed) + ".");
        res.set_content("Removed " + std::to_string(removed) + " file(s), " + HumanBytes(freed) + " freed.", kTextType);
    });
}

} // namespace swm
