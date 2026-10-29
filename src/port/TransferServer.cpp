/**
 * @file TransferServer.cpp
 * @brief Implementation of the asset/save transfer server (see TransferServer.h).
 *
 * HTTP API (all /api routes require the PIN, as an "X-Pin" header or a ?pin= query):
 *   GET  /                    the upload page (web/index.html, embedded at build time)
 *   GET  /api/status          JSON: mode, free space, data pack state, settings
 *   PUT  /api/data            raw body = Data.rsdk (streamed to disk)
 *   GET  /api/files           JSON: saves/settings files, Sonic Mania's and each retro game's
 *   GET  /api/files/:name[?game=<id>]   download one file (a retro game's with ?game=sonic1 ...)
 *   PUT  /api/files/:name[?game=<id>]   raw body = replacement file (applied at next launch while playing)
 *   PUT  /api/settings?plusEnabled=0|1&retroEnabled=0|1   (either or both)
 *   GET  /api/retro           JSON: retro mode, TV and USB space, each game's packs
 *   PUT  /api/retro/:game/:edition?dest=tv|usb[&mount=<USB mount point>]
 *                             raw body = a retro game's pack (edition: origins|classic)
 *   DELETE /api/retro/:game/:edition   removes that pack wherever it is
 *   POST /api/retro/usb-folder?mount=<USB mount point>   creates SonicWebMania/retro on the drive
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

#include <cstdio>
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
        const std::string packPath  = storage_.PathFor(Storage::kDataPackName);
        const bool packValid        = Storage::IsValidDataPackHeader(packPath);

        std::ostringstream json;
        json << "{\"version\":\"" << kAppVersion << "\""
             << ",\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"persistent\":" << (storage_.IsPersistent() ? "true" : "false")
             << ",\"dataPack\":{\"present\":" << (packValid ? "true" : "false")
             << ",\"size\":" << (packValid ? Storage::FileSize(packPath) : 0) << "}"
             << ",\"plusEnabled\":" << (config_.IsPlusEnabled() ? "true" : "false")
             << ",\"retroEnabled\":" << (config_.IsRetroEnabled() ? "true" : "false")
             << ",\"restartPending\":" << (status.restartPending ? "true" : "false")
             << ",\"upload\":{\"active\":" << (status.uploading ? "true" : "false")
             << ",\"received\":" << status.receivedBytes << ",\"expected\":" << status.expectedBytes << "}}";
        res.set_content(json.str(), kJsonType);
    });

    server.Put("/api/data", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected < kMinimumPackBytes)
            return Fail(res, 411, "The upload must state its size (Content-Length).");

        if (uploadInProgress_.exchange(true))
            return Fail(res, 409, "Another upload is already running.");
        // Clear the flag however this handler exits.
        const std::unique_ptr<std::atomic<bool>, void (*)(std::atomic<bool> *)> uploadGuard(
            &uploadInProgress_, [](std::atomic<bool> *flag) { flag->store(false); });

        const std::uint64_t needed = expected + Storage::kSafetyMarginBytes;
        const std::uint64_t free   = storage_.FreeBytes();
        if (needed > free) {
            return Fail(res, 507,
                        "Not enough space on the TV: this file needs " + HumanBytes(needed) + " (including a safety margin), "
                            + HumanBytes(free) + " is free.");
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading     = true;
            status_.receivedBytes = 0;
            status_.expectedBytes = expected;
        }
        PublishEvent("Receiving game data...");

        const std::string partPath = storage_.PathFor(std::string(Storage::kDataPackName) + ".part");
        const std::int64_t written = StreamBodyToFile(reader, partPath, expected, [this](std::uint64_t total) {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.receivedBytes = total;
        });

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.uploading = false;
        }

        std::error_code ec;
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

        const bool setup          = setupMode_.load();
        const std::string destination = setup ? storage_.PathFor(Storage::kDataPackName) : storage_.PendingDataPackPath();
        std::filesystem::rename(partPath, destination, ec);
        if (ec) {
            std::filesystem::remove(partPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            if (setup)
                status_.dataPackReady = true;
            else
                status_.restartPending = true;
        }
        PublishEvent(setup ? "Game data installed." : "New game data received. Restart to use it.");
        res.set_content(setup ? "installed" : "staged", kTextType);
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
        if (!req.has_param("plusEnabled") && !req.has_param("retroEnabled"))
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

        NoteRestartNeeded();
        PublishEvent(message);
        res.set_content("saved", kTextType);
    });

    RegisterRetroRoutes();
}

namespace
{

/** JSON for one pack slot of a retro game. */
std::string PackJson(const RetroPack &pack)
{
    std::ostringstream json;
    json << "{\"state\":\"" << PackStateName(pack.state) << "\"";
    if (pack.state != PackState::Missing) {
        json << ",\"location\":\"" << (pack.onUsb ? "usb" : "tv") << "\",\"path\":\"" << JsonEscape(pack.path)
             << "\",\"size\":" << pack.size;
        if (pack.version)
            json << ",\"version\":\"" << pack.version << "\"";
        json << ",\"standalone\":" << (pack.standalone ? "true" : "false");
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

/** Every folder a retro pack may be in: the TV's, then each USB drive's. */
std::vector<std::string> AllPackFolders(const RetroLibrary &retro)
{
    std::vector<std::string> folders = { retro.TvPackFolder() };
    for (const UsbDrive &drive : ListUsbDrives())
        folders.push_back(RetroLibrary::UsbPackFolder(drive.mountPoint));
    return folders;
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
                 << JsonEscape(game.ModeDescription()) << "\",\"origins\":" << PackJson(game.origins)
                 << ",\"classic\":" << PackJson(game.classic) << "}";
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
        const std::string label = PackLabel(*game, edition);
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

        const std::string destination = folder + "/" + fileName;
        std::filesystem::rename(readyPath, destination, ec);
        if (ec) {
            std::filesystem::remove(readyPath, ec);
            return Fail(res, 500, "Could not move the file into place.");
        }

        // One copy only: the TV's folder is searched first, so an older copy there would win.
        for (const std::string &other : AllPackFolders(retro_)) {
            if (other != folder)
                std::filesystem::remove(other + "/" + fileName, ec);
        }

        NoteRestartNeeded();
        PublishEvent(label + " installed.");
        res.set_content("installed", kTextType);
    });

    server.Delete("/api/retro/:game/:edition", [this](const httplib::Request &req, httplib::Response &res) {
        const RetroGameInfo *game = RetroLibrary::FindGame(req.path_params.at("game"));
        RetroEdition edition      = RetroEdition::Origins;
        if (!game || !ParseEdition(req.path_params.at("edition"), &edition))
            return Fail(res, 404, "Unknown game or edition.");

        const std::string fileName = RetroLibrary::PackFileName(game->id, edition);
        int removed                = 0;
        for (const std::string &folder : AllPackFolders(retro_)) {
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
}

} // namespace swm
