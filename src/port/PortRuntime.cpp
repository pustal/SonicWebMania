/**
 * @file PortRuntime.cpp
 * @brief Implementation of the in-game menu, notifications and engine hooks.
 */
#include "PortRuntime.h"

#include "NetInfo.h"
#include "TransferServer.h"

#include <algorithm>
#include <array>

namespace swm
{
namespace
{

/** The engine renders at 240 lines; layout is expressed in those units. */
constexpr int kBaseLines = 240;
/** Upper bound on how long we wait for a button release after closing the menu. */
constexpr std::uint32_t kReleaseTimeoutMs = 1000;
/** How long a notification stays on screen. */
constexpr std::uint32_t kToastDurationMs = 4000;
/** Frame pacing for the menu loop when vsync does not throttle us. */
constexpr std::uint32_t kMenuFrameDelayMs = 16;

constexpr Color kPanel     = { 10, 16, 40, 225 };
constexpr Color kBorder    = { 70, 120, 255, 255 };
constexpr Color kText      = { 235, 235, 245, 255 };
constexpr Color kDim       = { 150, 160, 190, 255 };
constexpr Color kHighlight = { 255, 210, 60, 255 };
constexpr Color kSelection = { 40, 70, 160, 255 };

constexpr std::array<const char *, 3> kMenuLabels = { "Resume", "Restart game", "Close app" };

/** True for events the engine must still receive after the menu closes. */
bool MustForwardAfterMenu(const SDL_Event &event)
{
    switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
        case SDL_WINDOWEVENT:
        case SDL_APP_WILLENTERBACKGROUND:
        case SDL_APP_WILLENTERFOREGROUND: return true;
        default: return false;
    }
}

} // namespace

PortRuntime &PortRuntime::Instance()
{
    static PortRuntime runtime;
    return runtime;
}

void PortRuntime::Configure(const TransferServer *server, std::string lanAddress, bool plusEnabled)
{
    server_      = server;
    lanAddress_  = std::move(lanAddress);
    plusEnabled_ = plusEnabled;
    if (server_)
        seenEventSerial_ = server_->Snapshot().eventSerial; // Do not replay setup-time events in game.
}

void PortRuntime::OnEngineVideoReady(SDL_Window *, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice, WebOSPortRedrawFn redraw)
{
    renderer_    = renderer;
    audioDevice_ = audioDevice;
    redraw_      = redraw;
    text_.Attach(renderer);
}

bool PortRuntime::FilterEvent(SDL_Event *event)
{
    if (!event)
        return false;

    if (RemoteInput::IsMenuOpenRequest(*event)) {
        RunMenu();
        return true;
    }

    // Swallow the release of the keys that open the menu as well.
    if (event->type == SDL_KEYUP) {
        SDL_Event asPress = *event;
        asPress.type      = SDL_KEYDOWN;
        asPress.key.repeat = 0;
        if (RemoteInput::IsMenuOpenRequest(asPress))
            return true;
    }

    RemoteInput::RewriteForEngine(event);
    return false;
}

void PortRuntime::RunMenu()
{
    if (menuOpen_ || !redraw_)
        return;

    menuOpen_   = true;
    selected_   = 0;
    lanAddress_ = FindLanAddress(); // The TV may have changed networks since launch.
    if (audioDevice_)
        SDL_PauseAudioDevice(audioDevice_, 1);

    std::vector<SDL_Event> forwarded;
    SDL_JoystickID closingController   = -1;
    SDL_GameControllerButton closingButton = SDL_CONTROLLER_BUTTON_INVALID;
    bool stickWasNeutral = true;

    while (menuOpen_ && !quitRequested_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || event.type == SDL_APP_TERMINATING) {
                quitRequested_ = true;
                break;
            }
            if (MustForwardAfterMenu(event)) {
                forwarded.push_back(event);
                continue;
            }

            MenuAction action = RemoteInput::ToMenuAction(event);
            if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                // Act once per stick push, not on every axis update.
                if (action == MenuAction::None) {
                    stickWasNeutral = true;
                }
                else if (!stickWasNeutral) {
                    action = MenuAction::None;
                }
                else {
                    stickWasNeutral = false;
                }
            }

            if (action != MenuAction::None && HandleMenuAction(action)) {
                if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                    closingController = event.cbutton.which;
                    closingButton     = static_cast<SDL_GameControllerButton>(event.cbutton.button);
                }
                break;
            }
        }

        redraw_();
        SDL_Delay(kMenuFrameDelayMs);
    }

    menuOpen_ = false;
    if (!quitRequested_) {
        if (closingController >= 0)
            WaitForControllerRelease(closingController, closingButton);
        if (audioDevice_)
            SDL_PauseAudioDevice(audioDevice_, 0);
    }

    for (SDL_Event &event : forwarded)
        SDL_PushEvent(&event);
}

bool PortRuntime::HandleMenuAction(MenuAction action)
{
    const int count = static_cast<int>(MenuItem::Count);
    switch (action) {
        case MenuAction::Up: selected_ = (selected_ + count - 1) % count; return false;
        case MenuAction::Down: selected_ = (selected_ + 1) % count; return false;
        case MenuAction::Cancel: menuOpen_ = false; return true;
        case MenuAction::Confirm:
            switch (static_cast<MenuItem>(selected_)) {
                case MenuItem::Resume: break;
                case MenuItem::Restart:
                    restartRequested_ = true;
                    quitRequested_    = true;
                    break;
                case MenuItem::Close: quitRequested_ = true; break;
                case MenuItem::Count: break;
            }
            menuOpen_ = false;
            return true;
        case MenuAction::None: return false;
    }
    return false;
}

void PortRuntime::WaitForControllerRelease(SDL_JoystickID controller, SDL_GameControllerButton button)
{
    SDL_GameController *pad = SDL_GameControllerFromInstanceID(controller);
    if (!pad || button == SDL_CONTROLLER_BUTTON_INVALID)
        return;

    const std::uint32_t start = SDL_GetTicks();
    while (SDL_GameControllerGetButton(pad, button) && SDL_GetTicks() - start < kReleaseTimeoutMs) {
        SDL_PumpEvents();
        SDL_Delay(kMenuFrameDelayMs);
    }
}

void PortRuntime::Layout(SDL_Renderer *renderer, int *width, int *height, int *scale) const
{
    SDL_RenderGetLogicalSize(renderer, width, height);
    if (*width <= 0 || *height <= 0)
        SDL_GetRendererOutputSize(renderer, width, height);
    *scale = std::max(1, *height / kBaseLines);
}

void PortRuntime::DrawOverlay(SDL_Renderer *renderer)
{
    if (!renderer || (renderer != renderer_ && !text_.Attach(renderer)))
        return;

    int width = 0, height = 0, scale = 1;
    Layout(renderer, &width, &height, &scale);

    if (menuOpen_)
        DrawMenu(width, height, scale);
    DrawToast(width, height, scale);
}

void PortRuntime::DrawMenu(int width, int height, int scale) const
{
    // Dim the frozen game frame behind the panel.
    text_.FillRect(0, 0, width, height, { 0, 0, 0, 140 });

    const std::string hint       = TransferHint();
    const bool restartPending    = server_ && server_->Snapshot().restartPending;
    const char *const kPendingMsg = "Uploaded files apply on restart";

    // Size the panel to its widest line, so it stays compact over the game.
    const int glyph   = BitmapFont::kGlyphSize * scale;
    const int lineGap = glyph + 6 * scale;
    int contentW      = TextRenderer::MeasureText(hint, scale);
    if (restartPending)
        contentW = std::max(contentW, TextRenderer::MeasureText(kPendingMsg, scale));
    for (const char *label : kMenuLabels)
        contentW = std::max(contentW, TextRenderer::MeasureText(label, scale));

    const int menuLines = static_cast<int>(MenuItem::Count);
    const int infoLines = restartPending ? 2 : 1;
    const int panelW    = std::min(width - 8 * scale, contentW + 24 * scale);
    const int panelH    = lineGap * (menuLines + infoLines) + lineGap * 3; // title, gaps and padding
    const int panelX    = (width - panelW) / 2;
    const int panelY    = (height - panelH) / 2;

    text_.FillRect(panelX - scale, panelY - scale, panelW + 2 * scale, panelH + 2 * scale, kBorder);
    text_.FillRect(panelX, panelY, panelW, panelH, kPanel);

    int y = panelY + lineGap / 2;
    text_.DrawTextCentered(width / 2, y, "PAUSED", scale, kHighlight);
    y += lineGap * 3 / 2;

    for (int i = 0; i < menuLines; ++i) {
        const bool selected = i == selected_;
        if (selected)
            text_.FillRect(panelX + 4 * scale, y - 3 * scale, panelW - 8 * scale, glyph + 6 * scale, kSelection);
        text_.DrawTextCentered(width / 2, y, kMenuLabels[static_cast<std::size_t>(i)], scale, selected ? kHighlight : kText);
        y += lineGap;
    }

    y += lineGap / 2;
    if (restartPending) {
        text_.DrawTextCentered(width / 2, y, kPendingMsg, scale, kHighlight);
        y += lineGap;
    }
    text_.DrawTextCentered(width / 2, y, hint, scale, kDim);
}

void PortRuntime::DrawToast(int width, int height, int scale)
{
    if (server_) {
        const TransferStatus status = server_->Snapshot();
        if (status.eventSerial != seenEventSerial_) {
            seenEventSerial_ = status.eventSerial;
            toastText_       = status.lastEvent;
            toastUntilTicks_ = SDL_GetTicks() + kToastDurationMs;
        }
        if (status.uploading && status.expectedBytes > 0) {
            const std::uint64_t percent = status.receivedBytes * 100 / status.expectedBytes;
            toastText_       = "Receiving game data: " + std::to_string(percent) + "%";
            toastUntilTicks_ = SDL_GetTicks() + kToastDurationMs;
        }
    }

    if (toastText_.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), toastUntilTicks_))
        return;

    const int glyph = BitmapFont::kGlyphSize * scale;
    const int textW = TextRenderer::MeasureText(toastText_, scale);
    const int boxW  = std::min(width - 8 * scale, textW + 8 * scale);
    const int boxX  = (width - boxW) / 2;
    const int boxY  = height - glyph - 12 * scale;
    text_.FillRect(boxX, boxY, boxW, glyph + 8 * scale, kPanel);
    text_.DrawTextCentered(width / 2, boxY + 4 * scale, toastText_, scale, kText);
}

std::string PortRuntime::TransferHint() const
{
    if (!server_ || server_->Port() == 0)
        return "File transfer unavailable";
    if (lanAddress_.empty())
        return "File transfer: TV is offline";
    return "http://" + lanAddress_ + ":" + std::to_string(server_->Port()) + "  PIN " + server_->Pin();
}

} // namespace swm

// ---- C hooks called from the patched engine ----

extern "C" void WebOSPort_OnEngineVideoReady(SDL_Window *window, SDL_Renderer *renderer, SDL_AudioDeviceID audioDevice,
                                             WebOSPortRedrawFn redraw)
{
    swm::PortRuntime::Instance().OnEngineVideoReady(window, renderer, audioDevice, redraw);
}

extern "C" int WebOSPort_FilterEvent(SDL_Event *event) { return swm::PortRuntime::Instance().FilterEvent(event) ? 1 : 0; }

extern "C" void WebOSPort_DrawOverlay(SDL_Renderer *renderer) { swm::PortRuntime::Instance().DrawOverlay(renderer); }

extern "C" int WebOSPort_ShouldQuit(void) { return swm::PortRuntime::Instance().ShouldQuit() ? 1 : 0; }

extern "C" int WebOSPort_IsPlusEnabled(void) { return swm::PortRuntime::Instance().IsPlusEnabled() ? 1 : 0; }
