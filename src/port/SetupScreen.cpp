/**
 * @file SetupScreen.cpp
 * @brief Implementation of the first-run setup screen (see SetupScreen.h).
 */
#include "SetupScreen.h"

#include "BitmapFont.h"
#include "NetInfo.h"
#include "PortInfo.h"
#include "RemoteInput.h"
#include "Storage.h"
#include "TransferServer.h"

#include <SDL.h>

#include <cstdio>
#include <string>

namespace swm
{
namespace
{

/** Logical canvas: divides 1080p and 2160p panels by whole numbers so text stays crisp. */
constexpr int kCanvasWidth  = 480;
constexpr int kCanvasHeight = 270;
constexpr int kLineHeight   = 12;
constexpr std::uint32_t kAddressRefreshMs = 2000;
constexpr std::uint32_t kStartDelayMs     = 1200;
constexpr std::uint32_t kFrameDelayMs     = 33;

constexpr Color kBackground = { 8, 14, 38, 255 };
constexpr Color kTitle      = { 255, 210, 60, 255 };
constexpr Color kText       = { 235, 235, 245, 255 };
constexpr Color kDim        = { 150, 160, 190, 255 };
constexpr Color kAccent     = { 90, 170, 255, 255 };
constexpr Color kWarning    = { 255, 120, 90, 255 };
constexpr Color kBarBack    = { 40, 50, 90, 255 };
constexpr Color kBarFill    = { 90, 200, 120, 255 };

/** Formats a byte count ("1.5 GB"). */
std::string HumanBytes(std::uint64_t bytes)
{
    constexpr const char *kUnits[] = { "B", "KB", "MB", "GB" };
    double value     = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f %s" : "%.1f %s", value, kUnits[unit]);
    return buffer;
}

/** Everything the screen draws, refreshed once per frame. */
struct ScreenState {
    std::string address;
    std::uint64_t freeBytes = 0;
    TransferStatus transfer;
    bool starting = false;
};

void DrawScreen(SDL_Renderer *renderer, const TextRenderer &text, const ScreenState &state, const Storage &storage,
                const TransferServer *server)
{
    SDL_SetRenderDrawColor(renderer, kBackground.r, kBackground.g, kBackground.b, kBackground.a);
    SDL_RenderClear(renderer);

    const int center = kCanvasWidth / 2;
    int y            = 18;

    text.DrawTextCentered(center, y, "SonicWebMania", 2, kTitle);
    y += 28;

    if (state.starting) {
        text.DrawTextCentered(center, kCanvasHeight / 2, "Game data installed. Starting...", 1, kText);
        SDL_RenderPresent(renderer);
        return;
    }

    text.DrawTextCentered(center, y, "The game data file (Data.rsdk) is needed.", 1, kText);
    y += kLineHeight;
    text.DrawTextCentered(center, y, "Use the one from your own copy of Sonic Mania.", 1, kDim);
    y += kLineHeight * 2;

    if (!server || server->Port() == 0) {
        text.DrawTextCentered(center, y, "The file transfer service could not start.", 1, kWarning);
        y += kLineHeight;
        text.DrawTextCentered(center, y, "Restart the app to try again.", 1, kDim);
        y += kLineHeight * 2;
    }
    else if (state.address.empty()) {
        text.DrawTextCentered(center, y, "Connect the TV to your network to continue.", 1, kWarning);
        y += kLineHeight * 3;
    }
    else {
        text.DrawTextCentered(center, y, "On a phone or computer on this network, open:", 1, kText);
        y += kLineHeight + 4;
        const std::string url = "http://" + state.address + ":" + std::to_string(server->Port());
        text.DrawTextCentered(center, y, url, 2, kAccent);
        y += 24;
        text.DrawTextCentered(center, y, "PIN  " + server->Pin(), 2, kTitle);
        y += 28;
    }

    const std::string space = "Free space on the TV: " + HumanBytes(state.freeBytes);
    text.DrawTextCentered(center, y, space, 1, kDim);
    y += kLineHeight;
    text.DrawTextCentered(center, y, "Needed: the size of your Data.rsdk + " + HumanBytes(Storage::kSafetyMarginBytes), 1, kDim);
    y += kLineHeight * 2;

    if (state.transfer.uploading && state.transfer.expectedBytes > 0) {
        constexpr int kBarWidth  = 300;
        constexpr int kBarHeight = 8;
        const int barX           = center - kBarWidth / 2;
        const int filled = static_cast<int>(state.transfer.receivedBytes * kBarWidth / state.transfer.expectedBytes);
        text.FillRect(barX, y, kBarWidth, kBarHeight, kBarBack);
        text.FillRect(barX, y, filled, kBarHeight, kBarFill);
        y += kBarHeight + 4;
        text.DrawTextCentered(center, y,
                              HumanBytes(state.transfer.receivedBytes) + " of " + HumanBytes(state.transfer.expectedBytes), 1,
                              kText);
    }
    else if (!state.transfer.lastEvent.empty()) {
        text.DrawTextCentered(center, y, state.transfer.lastEvent, 1, kText);
    }

    if (!storage.IsPersistent())
        text.DrawTextCentered(center, kCanvasHeight - 46, "Warning: using temporary storage (lost on reboot)", 1, kWarning);

    text.DrawTextCentered(center, kCanvasHeight - 30, "Press BACK to exit", 1, kDim);
    text.DrawTextCentered(center, kCanvasHeight - 16, std::string("v") + kAppVersion + "  " + kProjectUrlShort, 1, kAccent);
    SDL_RenderPresent(renderer);
}

} // namespace

SetupResult RunSetupScreen(const Storage &storage, const TransferServer *server)
{
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
        return SetupResult::Failed;

    SDL_Window *window = SDL_CreateWindow("SonicWebMania", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) {
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
        return SetupResult::Failed;
    }
    SDL_RenderSetLogicalSize(renderer, kCanvasWidth, kCanvasHeight);
    SDL_ShowCursor(SDL_DISABLE);

    // Open any connected pads so their buttons can be used to exit.
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i))
            SDL_GameControllerOpen(i);
    }

    TextRenderer text;
    text.Attach(renderer);

    ScreenState state;
    std::uint32_t nextAddressRefresh = 0;
    std::uint32_t startAt            = 0;
    SetupResult result               = SetupResult::Exit;
    bool running                     = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || event.type == SDL_APP_TERMINATING) {
                running = false;
            }
            else if (event.type == SDL_CONTROLLERDEVICEADDED) {
                SDL_GameControllerOpen(event.cdevice.which);
            }
            else if (RemoteInput::IsMenuOpenRequest(event) || RemoteInput::ToMenuAction(event) == MenuAction::Cancel) {
                running = false; // Back / Exit / B leave the app.
            }
        }

        const std::uint32_t now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, nextAddressRefresh)) {
            state.address      = FindLanAddress();
            state.freeBytes    = storage.FreeBytes();
            nextAddressRefresh = now + kAddressRefreshMs;
        }
        if (server)
            state.transfer = server->Snapshot();

        if (!state.starting && (state.transfer.dataPackReady || storage.HasValidDataPack())) {
            state.starting = true;
            startAt        = now + kStartDelayMs;
        }
        if (state.starting && SDL_TICKS_PASSED(now, startAt)) {
            result  = SetupResult::DataReady;
            running = false;
        }

        DrawScreen(renderer, text, state, storage, server);
        SDL_Delay(kFrameDelayMs);
    }

    text.Detach();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    return result;
}

} // namespace swm
