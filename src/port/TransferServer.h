/**
 * @file TransferServer.h
 * @brief A small HTTP server that lets a phone or computer on the same network
 *        upload the game data and download/upload save files, and manage
 *        the retro games' data packs (on the TV or a USB drive).
 *
 * Design notes for a TV:
 *  - Uploads are streamed straight to disk (never held in memory).
 *  - Free space is checked against the announced size before anything is written.
 *  - Only clients on private networks are answered, and every API call must
 *    carry the PIN shown on the TV screen.
 *  - Two worker threads at most; the game's main thread is never blocked.
 */
#ifndef SWM_TRANSFER_SERVER_H
#define SWM_TRANSFER_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib
{
class Server;
}

namespace swm
{

class Storage;
class PortConfig;
class RetroLibrary;

/** Snapshot of what the server is doing, for the TV screens to display. */
struct TransferStatus {
    bool uploading                 = false; ///< A Data.rsdk or retro pack upload is in progress.
    std::uint64_t receivedBytes    = 0;     ///< Bytes of the current/last upload written so far.
    std::uint64_t expectedBytes    = 0;     ///< Announced size of the current/last upload.
    bool dataPackReady             = false; ///< A valid Data.rsdk was installed (setup mode).
    bool restartPending            = false; ///< Files were staged that need a restart to apply.
    std::uint32_t eventSerial      = 0;     ///< Increments whenever lastEvent changes.
    std::string lastEvent;                  ///< Short human-readable description of the latest event.
};

class TransferServer
{
public:
    /** The game is not running yet: uploads are installed immediately. */
    static constexpr bool kSetupMode = true;

    /**
     * Port tried first. Chosen to stay clear of the ports usually taken by
     * other homebrew web servers and dev tools (80, 3000, 5000, 8000, 8080, 9000,
     * webOS' own 3000/3001/9922/9998). If it is busy, the next nine are tried.
     */
    static constexpr std::uint16_t kPreferredPort = 47801;

    TransferServer(Storage &storage, PortConfig &config, const RetroLibrary &retro, std::string pin);
    ~TransferServer();

    TransferServer(const TransferServer &)            = delete;
    TransferServer &operator=(const TransferServer &) = delete;

    /**
     * Binds the first free port in [kPreferredPort, kPreferredPort + 9] and
     * starts serving on a background thread.
     *
     * @return False when no port could be bound.
     */
    bool Start();

    /** Stops the server and joins its thread. Safe to call more than once. */
    void Stop();

    /** Switches between setup mode (install now) and in-game mode (stage for next launch). */
    void SetSetupMode(bool setupMode) { setupMode_.store(setupMode); }

    /** The bound port, or 0 when not running. */
    std::uint16_t Port() const { return port_.load(); }

    /** The PIN clients must send. */
    const std::string &Pin() const { return pin_; }

    /** Thread-safe copy of the current status. */
    TransferStatus Snapshot() const;

private:
    void RegisterRoutes();

    /** The /api/retro routes (see TransferServer.cpp). */
    void RegisterRetroRoutes();

    /** Marks that something changed which applies at the next launch (only while a game runs). */
    void NoteRestartNeeded();

    /** Records a status event shown on the TV. */
    void PublishEvent(const std::string &message);

    Storage &storage_;
    PortConfig &config_;
    const RetroLibrary &retro_;
    const std::string pin_;

    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::atomic<std::uint16_t> port_{ 0 };
    std::atomic<bool> setupMode_{ kSetupMode };
    std::atomic<bool> uploadInProgress_{ false };

    mutable std::mutex statusMutex_;
    TransferStatus status_;
};

/** Generates a random 4-digit PIN for this session. */
std::string GeneratePin();

} // namespace swm

#endif // SWM_TRANSFER_SERVER_H
