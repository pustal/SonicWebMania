/**
 * @file PortRuntime.cpp
 * @brief Implementation of the in-game menu, notifications and engine hooks.
 *
 * The menu has four pages:
 *   - Main: Resume, Achievements, Button test, Retro games (when there is more
 *     than one game to play), Restart game, Close app, plus the transfer
 *     address and the project page;
 *   - Retro games: Sonic Mania and the retro games whose packs are installed
 *     (see RetroGames.h); choosing one restarts the app into it;
 *   - Achievements: the engine's achievement list, unlocked ones highlighted,
 *     with the selected one's description;
 *   - Button test: shows what each remote or controller press is and which
 *     game button it becomes, to check the TV's remote mapping.
 */
#include "PortRuntime.h"

#include "NetInfo.h"
#include "PortInfo.h"
#include "TransferServer.h"

#include <algorithm>
#include <array>
#include <utility>

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
/**
 * Shortest press the game is guaranteed to see. A quick tap on the remote can
 * deliver key-down and key-up before the engine reads its input; holding the
 * release back this long makes the press last at least a couple of frames.
 */
constexpr std::uint32_t kMinRemotePressMs = 50;
/** Rows of the achievement list visible at once. */
constexpr int kAchievementRows = 8;
/** Presses remembered on the button test page. */
constexpr std::size_t kButtonTestHistory = 5;

constexpr Color kPanel     = { 10, 16, 40, 225 };
constexpr Color kBorder    = { 70, 120, 255, 255 };
constexpr Color kText      = { 235, 235, 245, 255 };
constexpr Color kDim       = { 150, 160, 190, 255 };
constexpr Color kHighlight = { 255, 210, 60, 255 };
constexpr Color kSelection = { 40, 70, 160, 255 };
constexpr Color kLink      = { 90, 170, 255, 255 };
constexpr Color kUnlocked  = { 90, 200, 120, 255 };
constexpr Color kDimmer    = { 0, 0, 0, 140 };


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
        case SDL_APP_WILLENTERFOREGROUND:
        // Releases too: a key held when the menu opened must not stay pressed in the game.
        case SDL_KEYUP: return true;
        default: return false;
    }
}

/**
 * Splits @p text into lines of at most @p maxChars characters, breaking at
 * spaces where possible.
 */
std::vector<std::string> WrapText(const std::string &text, std::size_t maxChars)
{
    std::vector<std::string> lines;
    std::string remaining = text;
    while (remaining.size() > maxChars) {
        std::size_t cut = remaining.rfind(' ', maxChars);
        if (cut == std::string::npos || cut == 0)
            cut = maxChars;
        lines.push_back(remaining.substr(0, cut));
        remaining = remaining.substr(remaining[cut] == ' ' ? cut + 1 : cut);
    }
    if (!remaining.empty())
        lines.push_back(remaining);
    return lines;
}

} // namespace

PortRuntime &PortRuntime::Instance()
{
    static PortRuntime runtime;
    return runtime;
}

void PortRuntime::Configure(const TransferServer *server, std::string lanAddress, bool plusEnabled, AchievementProvider achievements)
{
    server_              = server;
    lanAddress_          = std::move(lanAddress);
    plusEnabled_         = plusEnabled;
    achievementProvider_ = achievements;
    if (server_)
        seenEventSerial_ = server_->Snapshot().eventSerial; // Do not replay setup-time events in game.
}

void PortRuntime::SetGames(std::vector<GameChoice> games, std::string currentId)
{
    games_       = std::move(games);
    currentGame_ = std::move(currentId);
    nextGame_.clear();
    RebuildMenu();
}

void PortRuntime::RebuildMenu()
{
    menuItems_ = { MenuItem::Resume, MenuItem::Achievements, MenuItem::ButtonTest };
    if (games_.size() > 1)
        menuItems_.push_back(MenuItem::Games);
    menuItems_.push_back(MenuItem::Restart);
    menuItems_.push_back(MenuItem::Close);
}

const char *PortRuntime::Label(MenuItem item)
{
    switch (item) {
        case MenuItem::Resume: return "Resume";
        case MenuItem::Achievements: return "Achievements";
        case MenuItem::ButtonTest: return "Button test";
        case MenuItem::Games: return "Retro games";
        case MenuItem::Restart: return "Restart game";
        case MenuItem::Close: return "Close app";
    }
    return "";
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
        SDL_Event asPress  = *event;
        asPress.type       = SDL_KEYDOWN;
        asPress.key.repeat = 0;
        if (RemoteInput::IsMenuOpenRequest(asPress))
            return true;
    }

    if (!RemoteInput::RewriteForEngine(event))
        return false;

    // A remote button: make sure the game sees the press for at least a frame or two.
    const auto scancode = static_cast<std::size_t>(event->key.keysym.scancode);
    if (scancode >= remotePressTicks_.size())
        return false;

    const std::uint32_t now = SDL_GetTicks();
    if (event->type == SDL_KEYDOWN) {
        remotePressTicks_[scancode] = now;
        return false;
    }

    const std::uint32_t pressedAt = remotePressTicks_[scancode];
    if (now - pressedAt < kMinRemotePressMs) {
        pendingReleases_.push_back(PendingRelease{ *event, pressedAt + kMinRemotePressMs });
        return true; // Re-queued by FlushPendingReleases() once due.
    }
    return false;
}

void PortRuntime::FlushPendingReleases()
{
    if (pendingReleases_.empty())
        return;

    const std::uint32_t now = SDL_GetTicks();
    auto due = std::stable_partition(pendingReleases_.begin(), pendingReleases_.end(),
                                     [now](const PendingRelease &pending) { return !SDL_TICKS_PASSED(now, pending.dueTicks); });
    for (auto it = due; it != pendingReleases_.end(); ++it) {
        // Already rewritten to an engine key, so FilterEvent passes it through unchanged.
        SDL_Event release = it->event;
        remotePressTicks_[static_cast<std::size_t>(release.key.keysym.scancode)] = 0;
        SDL_PushEvent(&release);
    }
    pendingReleases_.erase(due, pendingReleases_.end());
}

void PortRuntime::RunMenu()
{
    if (menuOpen_ || !redraw_)
        return;

    menuOpen_   = true;
    screen_     = Screen::Main;
    selected_   = 0;
    lanAddress_ = FindLanAddress(); // The TV may have changed networks since launch.
    if (audioDevice_)
        SDL_PauseAudioDevice(audioDevice_, 1);

    std::vector<SDL_Event> forwarded;
    SDL_JoystickID closingController       = -1;
    SDL_GameControllerButton closingButton = SDL_CONTROLLER_BUTTON_INVALID;
    bool stickWasNeutral                   = true;

    while (menuOpen_ && !quitRequested_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT || event.type == SDL_APP_TERMINATING) {
                quitRequested_ = true;
                break;
            }
            if (MustForwardAfterMenu(event)) {
                if (event.type == SDL_KEYUP)
                    RemoteInput::RewriteForEngine(&event);
                forwarded.push_back(event);
                continue;
            }

            if (HandleMenuEvent(event, &stickWasNeutral)) {
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

bool PortRuntime::HandleMenuEvent(const SDL_Event &event, bool *stickWasNeutral)
{
    if (screen_ == Screen::ButtonTest) {
        HandleButtonTestEvent(event);
        return false;
    }

    MenuAction action = RemoteInput::ToMenuAction(event);
    if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        // Act once per stick push, not on every axis update.
        if (action == MenuAction::None)
            *stickWasNeutral = true;
        else if (!*stickWasNeutral)
            action = MenuAction::None;
        else
            *stickWasNeutral = false;
    }
    if (action == MenuAction::None)
        return false;

    if (screen_ == Screen::Achievements) {
        HandleAchievementsAction(action);
        return false;
    }
    if (screen_ == Screen::Games)
        return HandleGamesAction(action);
    return HandleMainAction(action);
}

bool PortRuntime::HandleMainAction(MenuAction action)
{
    const int count = static_cast<int>(menuItems_.size());
    switch (action) {
        case MenuAction::Up: selected_ = (selected_ + count - 1) % count; return false;
        case MenuAction::Down: selected_ = (selected_ + 1) % count; return false;
        case MenuAction::Cancel: menuOpen_ = false; return true;
        case MenuAction::Confirm:
            switch (menuItems_[static_cast<std::size_t>(selected_)]) {
                case MenuItem::Resume: menuOpen_ = false; return true;
                case MenuItem::Achievements:
                    achievements_        = achievementProvider_ ? achievementProvider_() : std::vector<AchievementEntry>{};
                    achievementSelected_ = 0;
                    achievementTop_      = 0;
                    screen_              = Screen::Achievements;
                    return false;
                case MenuItem::ButtonTest:
                    buttonTestLog_.clear();
                    screen_ = Screen::ButtonTest;
                    return false;
                case MenuItem::Games: {
                    // Start on the running game.
                    gameSelected_ = 0;
                    for (std::size_t i = 0; i < games_.size(); ++i) {
                        if (games_[i].id == currentGame_)
                            gameSelected_ = static_cast<int>(i);
                    }
                    screen_ = Screen::Games;
                    return false;
                }
                case MenuItem::Restart:
                    nextGame_.clear(); // Same game.
                    restartRequested_ = true;
                    quitRequested_    = true;
                    menuOpen_         = false;
                    return true;
                case MenuItem::Close:
                    quitRequested_ = true;
                    menuOpen_      = false;
                    return true;
            }
            return false;
        case MenuAction::None: return false;
    }
    return false;
}

void PortRuntime::HandleAchievementsAction(MenuAction action)
{
    const int count = static_cast<int>(achievements_.size());
    switch (action) {
        case MenuAction::Up:
            if (count > 0)
                achievementSelected_ = (achievementSelected_ + count - 1) % count;
            break;
        case MenuAction::Down:
            if (count > 0)
                achievementSelected_ = (achievementSelected_ + 1) % count;
            break;
        case MenuAction::Cancel:
        case MenuAction::Confirm: screen_ = Screen::Main; return;
        case MenuAction::None: return;
    }

    // Keep the selection inside the visible window.
    if (achievementSelected_ < achievementTop_)
        achievementTop_ = achievementSelected_;
    else if (achievementSelected_ >= achievementTop_ + kAchievementRows)
        achievementTop_ = achievementSelected_ - kAchievementRows + 1;
}

bool PortRuntime::HandleGamesAction(MenuAction action)
{
    const int count = static_cast<int>(games_.size());
    switch (action) {
        case MenuAction::Up:
            if (count > 0)
                gameSelected_ = (gameSelected_ + count - 1) % count;
            return false;
        case MenuAction::Down:
            if (count > 0)
                gameSelected_ = (gameSelected_ + 1) % count;
            return false;
        case MenuAction::Cancel: screen_ = Screen::Main; return false;
        case MenuAction::Confirm: {
            if (count == 0)
                return false;
            const GameChoice &choice = games_[static_cast<std::size_t>(gameSelected_)];
            if (choice.id == currentGame_) {
                screen_ = Screen::Main; // Already playing it.
                return false;
            }
            // A fresh process is the only clean way to switch engine and data.
            nextGame_         = choice.id;
            restartRequested_ = true;
            quitRequested_    = true;
            menuOpen_         = false;
            return true;
        }
        case MenuAction::None: return false;
    }
    return false;
}

void PortRuntime::HandleButtonTestEvent(const SDL_Event &event)
{
    // Back, Exit or the pad's View/Guide leave the test; every other button is just shown.
    if (RemoteInput::IsMenuOpenRequest(event)) {
        screen_ = Screen::Main;
        return;
    }

    const std::string description = RemoteInput::DescribePress(event);
    if (description.empty())
        return;

    buttonTestLog_.push_front(description);
    if (buttonTestLog_.size() > kButtonTestHistory)
        buttonTestLog_.pop_back();
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
    // Called once per frame by the engine, which makes it the right place to
    // release held-back remote keys.
    FlushPendingReleases();

    if (!renderer || (renderer != renderer_ && !text_.Attach(renderer)))
        return;

    int width = 0, height = 0, scale = 1;
    Layout(renderer, &width, &height, &scale);

    if (menuOpen_)
        DrawMenu(width, height, scale);
    DrawToast(width, height, scale);
}

SDL_Rect PortRuntime::DrawPanel(int width, int height, int panelW, int panelH, int scale) const
{
    const SDL_Rect panel{ (width - panelW) / 2, (height - panelH) / 2, panelW, panelH };
    text_.FillRect(panel.x - scale, panel.y - scale, panel.w + 2 * scale, panel.h + 2 * scale, kBorder);
    text_.FillRect(panel.x, panel.y, panel.w, panel.h, kPanel);
    return panel;
}

void PortRuntime::DrawMenu(int width, int height, int scale) const
{
    // Dim the frozen game frame behind the panel.
    text_.FillRect(0, 0, width, height, kDimmer);

    switch (screen_) {
        case Screen::Main: DrawMainPage(width, height, scale); break;
        case Screen::Achievements: DrawAchievementsPage(width, height, scale); break;
        case Screen::ButtonTest: DrawButtonTestPage(width, height, scale); break;
        case Screen::Games: DrawGamesPage(width, height, scale); break;
    }
}

void PortRuntime::DrawMainPage(int width, int height, int scale) const
{
    const std::string hint        = TransferHint();
    const bool restartPending     = server_ && server_->Snapshot().restartPending;
    const char *const kPendingMsg = "Uploaded files apply on restart";
    // The TV cannot open links, so the project page is shown as text to type or search.
    const std::string about = std::string("v") + kAppVersion + "  " + kProjectUrlShort;

    // Size the panel to its widest line, so it stays compact over the game.
    const int glyph   = BitmapFont::kGlyphSize * scale;
    const int lineGap = glyph + 5 * scale;
    int contentW      = std::max(TextRenderer::MeasureText(hint, scale), TextRenderer::MeasureText(about, scale));
    if (restartPending)
        contentW = std::max(contentW, TextRenderer::MeasureText(kPendingMsg, scale));
    for (const MenuItem item : menuItems_)
        contentW = std::max(contentW, TextRenderer::MeasureText(Label(item), scale));

    const int menuLines = static_cast<int>(menuItems_.size());
    const int infoLines = restartPending ? 3 : 2; // [pending], transfer hint, about
    const int panelW    = std::min(width - 8 * scale, contentW + 24 * scale);
    const int panelH    = lineGap * (menuLines + infoLines) + lineGap * 3; // title, gaps and padding
    const SDL_Rect panel = DrawPanel(width, height, panelW, panelH, scale);

    int y = panel.y + lineGap / 2;
    text_.DrawTextCentered(width / 2, y, "PAUSED", scale, kHighlight);
    y += lineGap * 3 / 2;

    for (int i = 0; i < menuLines; ++i) {
        const bool selected = i == selected_;
        if (selected)
            text_.FillRect(panel.x + 4 * scale, y - 3 * scale, panel.w - 8 * scale, glyph + 6 * scale, kSelection);
        text_.DrawTextCentered(width / 2, y, Label(menuItems_[static_cast<std::size_t>(i)]), scale, selected ? kHighlight : kText);
        y += lineGap;
    }

    y += lineGap / 2;
    if (restartPending) {
        text_.DrawTextCentered(width / 2, y, kPendingMsg, scale, kHighlight);
        y += lineGap;
    }
    text_.DrawTextCentered(width / 2, y, hint, scale, kDim);
    y += lineGap;
    text_.DrawTextCentered(width / 2, y, about, scale, kLink);
}

void PortRuntime::DrawAchievementsPage(int width, int height, int scale) const
{
    const int glyph    = BitmapFont::kGlyphSize * scale;
    const int lineGap  = glyph + 5 * scale;
    const int pad      = 8 * scale;
    const int panelW   = std::min(width - 8 * scale, 52 * glyph);
    const int maxChars = (panelW - 2 * pad) / glyph;
    const int panelH   = lineGap * (kAchievementRows + 6) + pad;
    const SDL_Rect panel = DrawPanel(width, height, panelW, panelH, scale);

    const int unlocked = static_cast<int>(
        std::count_if(achievements_.begin(), achievements_.end(), [](const AchievementEntry &a) { return a.unlocked; }));
    const int total = static_cast<int>(achievements_.size());

    int y = panel.y + pad;
    text_.DrawText(panel.x + pad, y, "ACHIEVEMENTS", scale, kHighlight);
    const std::string counter = std::to_string(unlocked) + "/" + std::to_string(total) + " unlocked";
    text_.DrawText(panel.x + panel.w - pad - TextRenderer::MeasureText(counter, scale), y, counter, scale, kDim);
    y += lineGap * 3 / 2;

    if (achievements_.empty()) {
        text_.DrawTextCentered(width / 2, y + lineGap * 3, "No achievements available", scale, kDim);
    }

    const int last = std::min(total, achievementTop_ + kAchievementRows);
    for (int i = achievementTop_; i < last; ++i) {
        const AchievementEntry &entry = achievements_[static_cast<std::size_t>(i)];
        const bool selected           = i == achievementSelected_;
        if (selected)
            text_.FillRect(panel.x + 4 * scale, y - 2 * scale, panel.w - 8 * scale, glyph + 4 * scale, kSelection);

        // A filled box marks an unlocked achievement; locked ones are dimmed.
        const Color markColor = entry.unlocked ? kUnlocked : kDim;
        text_.FillRect(panel.x + pad, y + scale, glyph - 2 * scale, glyph - 2 * scale, markColor);
        if (!entry.unlocked)
            text_.FillRect(panel.x + pad + scale, y + 2 * scale, glyph - 4 * scale, glyph - 4 * scale, kPanel);

        std::string name = entry.name;
        if (static_cast<int>(name.size()) > maxChars - 2)
            name = name.substr(0, static_cast<std::size_t>(maxChars - 3)) + ".";
        const Color nameColor = selected ? kHighlight : (entry.unlocked ? kText : kDim);
        text_.DrawText(panel.x + pad + glyph + 4 * scale, y, name, scale, nameColor);
        y += lineGap;
    }

    // Scroll hints when the list continues above or below.
    const int listTop = panel.y + pad + lineGap * 3 / 2;
    if (achievementTop_ > 0)
        text_.DrawText(panel.x + panel.w - pad - glyph, listTop, "^", scale, kDim);
    if (last < total)
        text_.DrawText(panel.x + panel.w - pad - glyph, listTop + lineGap * (kAchievementRows - 1), "v", scale, kDim);

    // Details of the selected achievement.
    y = listTop + lineGap * kAchievementRows + lineGap / 2;
    if (total > 0) {
        const AchievementEntry &entry = achievements_[static_cast<std::size_t>(achievementSelected_)];
        text_.DrawText(panel.x + pad, y, entry.unlocked ? "Unlocked" : "Locked", scale, entry.unlocked ? kUnlocked : kDim);
        y += lineGap;
        const std::vector<std::string> lines = WrapText(entry.description, static_cast<std::size_t>(maxChars));
        for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
            text_.DrawText(panel.x + pad, y, lines[i], scale, kText);
            y += lineGap;
        }
    }

    text_.DrawTextCentered(width / 2, panel.y + panel.h - pad - glyph, "BACK: return", scale, kDim);
}

void PortRuntime::DrawButtonTestPage(int width, int height, int scale) const
{
    const int glyph   = BitmapFont::kGlyphSize * scale;
    const int lineGap = glyph + 5 * scale;
    const int pad     = 8 * scale;
    const int panelW  = std::min(width - 8 * scale, 52 * glyph);
    const int panelH  = lineGap * (static_cast<int>(kButtonTestHistory) + 6) + pad;
    const SDL_Rect panel = DrawPanel(width, height, panelW, panelH, scale);

    int y = panel.y + pad;
    text_.DrawTextCentered(width / 2, y, "BUTTON TEST", scale, kHighlight);
    y += lineGap * 3 / 2;
    text_.DrawTextCentered(width / 2, y, "Press remote or controller buttons", scale, kText);
    y += lineGap * 3 / 2;

    if (buttonTestLog_.empty()) {
        text_.DrawTextCentered(width / 2, y, "Waiting for a press...", scale, kDim);
    }
    for (std::size_t i = 0; i < buttonTestLog_.size(); ++i) {
        text_.DrawTextCentered(width / 2, y, buttonTestLog_[i], scale, i == 0 ? kHighlight : kDim);
        y += lineGap;
    }

    text_.DrawTextCentered(width / 2, panel.y + panel.h - pad - glyph, "BACK: return", scale, kDim);
}

void PortRuntime::DrawGamesPage(int width, int height, int scale) const
{
    const int glyph    = BitmapFont::kGlyphSize * scale;
    const int lineGap  = glyph + 5 * scale;
    const int pad      = 8 * scale;
    const int panelW   = std::min(width - 8 * scale, 52 * glyph);
    const int maxChars = (panelW - 2 * pad) / glyph;
    const int rows     = static_cast<int>(games_.size());
    const int panelH   = lineGap * (rows + 6) + pad;
    const SDL_Rect panel = DrawPanel(width, height, panelW, panelH, scale);

    int y = panel.y + pad;
    text_.DrawTextCentered(width / 2, y, "RETRO GAMES", scale, kHighlight);
    y += lineGap * 3 / 2;

    for (int i = 0; i < rows; ++i) {
        const GameChoice &choice = games_[static_cast<std::size_t>(i)];
        const bool selected      = i == gameSelected_;
        if (selected)
            text_.FillRect(panel.x + 4 * scale, y - 2 * scale, panel.w - 8 * scale, glyph + 4 * scale, kSelection);
        const std::string label = choice.id == currentGame_ ? choice.title + "  (playing)" : choice.title;
        text_.DrawTextCentered(width / 2, y, label, scale, selected ? kHighlight : kText);
        y += lineGap;
    }

    // How the selected game will play (with or without sound, which release).
    y += lineGap / 2;
    if (rows > 0) {
        const std::vector<std::string> lines =
            WrapText(games_[static_cast<std::size_t>(gameSelected_)].note, static_cast<std::size_t>(maxChars));
        for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
            text_.DrawTextCentered(width / 2, y, lines[i], scale, kDim);
            y += lineGap;
        }
    }

    text_.DrawTextCentered(width / 2, panel.y + panel.h - pad - glyph, "OK: play   BACK: return", scale, kDim);
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
            toastText_                  = "Receiving game data: " + std::to_string(percent) + "%";
            toastUntilTicks_            = SDL_GetTicks() + kToastDurationMs;
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
