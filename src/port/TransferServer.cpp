/**
 * @file TransferServer.cpp
 * @brief Implementation of the asset/save transfer server (see TransferServer.h).
 *
 * HTTP API (all /api routes require the PIN, as an "X-Pin" header or a ?pin= query):
 *   GET  /                    the upload page (web/index.html, embedded at build time)
 *   GET  /api/status          JSON: mode, free space, data pack state, settings
 *   PUT  /api/data            raw body = Data.rsdk (streamed to disk)
 *   GET  /api/files           JSON list of saves/settings files
 *   GET  /api/files/:name     download one file
 *   PUT  /api/files/:name     raw body = replacement file (applied at next launch while playing)
 *   PUT  /api/settings?plusEnabled=0|1
 */
#include "TransferServer.h"

#include "NetInfo.h"
#include "PortConfig.h"
#include "Storage.h"
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
#include <system_error>
#include <filesystem>

namespace swm
{
namespace
{

constexpr const char *kJsonType   = "application/json";
constexpr const char *kTextType   = "text/plain; charset=utf-8";
constexpr const char *kPinHeader  = "X-Pin";
constexpr int kPortAttempts       = 10;
constexpr std::uint64_t kMinimumPackBytes = 64; // Header plus a file table; anything smaller is not a pack.

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

} // namespace

std::string GeneratePin()
{
    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 9999);
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", digits(device));
    return buffer;
}

TransferServer::TransferServer(Storage &storage, PortConfig &config, std::string pin)
    : storage_(storage), config_(config), pin_(std::move(pin))
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
        json << "{\"mode\":\"" << (setupMode_.load() ? "setup" : "game") << "\""
             << ",\"freeBytes\":" << storage_.FreeBytes() << ",\"safetyMarginBytes\":" << Storage::kSafetyMarginBytes
             << ",\"persistent\":" << (storage_.IsPersistent() ? "true" : "false")
             << ",\"dataPack\":{\"present\":" << (packValid ? "true" : "false")
             << ",\"size\":" << (packValid ? Storage::FileSize(packPath) : 0) << "}"
             << ",\"plusEnabled\":" << (config_.IsPlusEnabled() ? "true" : "false")
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
        json << "{\"files\":[";
        bool first = true;
        for (const UserFile &file : storage_.ListUserFiles()) {
            json << (first ? "" : ",") << "{\"name\":\"" << JsonEscape(file.name) << "\",\"size\":" << file.size
                 << ",\"mtime\":" << file.mtime << "}";
            first = false;
        }
        json << "]}";
        res.set_content(json.str(), kJsonType);
    });

    server.Get("/api/files/:name", [this](const httplib::Request &req, httplib::Response &res) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedUserFileName(name))
            return Fail(res, 400, "That file name is not allowed.");

        const std::string path = storage_.PathFor(name);
        const std::uint64_t size = Storage::FileSize(path);
        if (size == 0 || size > Storage::kMaxUserFileBytes)
            return Fail(res, 404, "File not found.");

        std::ifstream in(path, std::ios::binary);
        std::string body(static_cast<std::size_t>(size), '\0');
        if (!in.read(&body[0], static_cast<std::streamsize>(size)))
            return Fail(res, 500, "Could not read the file.");

        res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
        res.set_content(std::move(body), "application/octet-stream");
    });

    server.Put("/api/files/:name", [this](const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader) {
        const std::string name = req.path_params.at("name");
        if (!Storage::IsAllowedUserFileName(name))
            return Fail(res, 400, "Only .bin save files and .ini settings files can be uploaded.");

        std::uint64_t expected = 0;
        if (!ContentLength(req, &expected) || expected == 0)
            return Fail(res, 411, "The upload must state its size (Content-Length).");
        if (expected > Storage::kMaxUserFileBytes)
            return Fail(res, 413, "Save files are never this big; refusing " + HumanBytes(expected) + ".");
        if (expected + Storage::kSafetyMarginBytes > storage_.FreeBytes())
            return Fail(res, 507, "Not enough space on the TV.");

        // While playing, the engine owns its save files: stage the upload for the next launch.
        const bool setup = setupMode_.load();
        std::error_code ec;
        if (!setup)
            std::filesystem::create_directories(storage_.IncomingDir(), ec);
        const std::string destination = setup ? storage_.PathFor(name) : storage_.IncomingDir() + "/" + name;
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
        PublishEvent(setup ? name + " uploaded." : name + " received. Restart to use it.");
        res.set_content(setup ? "installed" : "staged", kTextType);
    });

    server.Put("/api/settings", [this](const httplib::Request &req, httplib::Response &res) {
        if (!req.has_param("plusEnabled"))
            return Fail(res, 400, "Nothing to change.");

        const bool enabled = req.get_param_value("plusEnabled") == "1";
        if (!config_.SetPlusEnabled(enabled))
            return Fail(res, 500, "Could not save the setting.");

        {
            std::lock_guard<std::mutex> lock(statusMutex_);
            status_.restartPending = status_.restartPending || !setupMode_.load();
        }
        PublishEvent(enabled ? "Plus content enabled. Restart to apply." : "Plus content disabled. Restart to apply.");
        res.set_content("saved", kTextType);
    });
}

} // namespace swm
