/**
 * @file RetroSaves.cpp
 * @brief Save slots for the retro games: the engine side of the save screen
 *        (see port/SaveSelect.h), behind the WebOSPort_OnLegacyStageLoad hook.
 *
 * Built with the engine (it reads the engine's scene list, global variables
 * and save memory), unlike the rest of the port.
 *
 * With only their data packs, the retro games go from the title screen
 * straight into the first zone: the 2013 releases of Sonic 1 and 2 drew their
 * save slots in native code, and Sonic Origins draws them in its own program.
 * The games' scripts still save progress the 2013 way, though, into the
 * engine's save memory (SGame.bin): 8 values per slot, slot = options.saveSlot,
 *   0 character, 1 lives, 2 score, 3 score bonus,
 *   4 stage (regular stage list position + 1; 0x80+ = special stage),
 *   5 emeralds (bit mask), 6 special stage position, 7 unused,
 * written whenever options.gameMode is 1. (Same layout in the Origins and the
 * 2013 packs; see RSDKv4-Decompilation's Userdata.hpp and NativeObjects/.)
 *
 * So when a game leaves its title screen for its first zone:
 *   - Sonic 1 and 2 (RSDKv4): the save screen is shown over the frozen title
 *     and its choice is applied as the 2013 menus did: the globals the scripts
 *     read, the character and, to continue, the saved stage. The 2013 packs'
 *     title screen cannot start a game by itself (the 2013 releases started
 *     games from their native menus; the script title only runs the demo), so
 *     for them the save screen takes the demo's place: it opens when the title
 *     would start the demo;
 *   - Sonic CD (RSDKv3): its packs still contain the 2011 release's own menu
 *     (with its save slots, Time Attack and extras), so the game goes there.
 * With the Origins packs, whose title starts games itself, the demo (attract
 * mode) is left alone.
 *
 * Sonic Origins' modes: its scripts read game.playMode (0 Classic,
 * 1 Anniversary, 2 Boss Rush, 3 Mirror, ...) and game.coinMode (1: an extra
 * life becomes a coin, NOTIFY_ADD_COIN, which the engine counts in
 * game.coinCount). With the Origins packs the save screen starts with a mode
 * page; the coin count is kept in a file shared by the three games, as Origins
 * shares it; Mirror mode is done by the port (WebOSPort_IsMirrored): the
 * legacy cores mirror the stage before drawing the HUD and text (patch 0004)
 * and swap left and right; Boss Rush
 * starts at the pack's first Boss Rush stage (folder "BR1..."). Each Boss
 * Rush stage is a whole act: Origins starts the players at its last
 * checkpoint, before the boss, by setting lampPostID (the lamp post's object
 * then moves the players to itself when the stage loads, also when retrying);
 * the port sets it once the stage's objects have loaded and loads the stage
 * again (WebOSPort_OnLegacyStageLoaded).
 *
 * Achievements: the engine registers Sonic Mania's list at start-up; for a
 * retro game it is replaced with that game's (port/RetroAchievements.h) and
 * the game folder's Achievements.bin is read into it. The scripts' unlocks
 * (WebOSPort_OnLegacyAchievement) mark the list, save the file and show a
 * notice; Sonic CD's menu gets its Achievements entry back (it is offered
 * when engine.onlineActive is set) and opens the Back menu's list.
 */
#include "RSDK/Core/RetroEngine.hpp"

#include "RetroSaves.h"

#include "port/PortRuntime.h"
#include "port/RetroAchievements.h"
#include "port/SaveSelect.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{

/** Values per save slot in the save memory. */
constexpr int kSlotValues = 8;
/** Offsets inside a slot. */
constexpr int kCharacter = 0, kLives = 1, kScore = 2, kScoreBonus = 3, kStage = 4, kEmeralds = 5, kSpecialStage = 6;
/** A stage value at or above this is a special stage. */
constexpr int kSpecialStageFlag = 0x80;
/** A new game's values (the 2013 menu's). */
constexpr int kNewLives = 3, kNewScoreBonus = 50000;

/** Most coins Origins counts (its HUD has three digits). */
constexpr int kMaxCoins = 999;
/** Folder prefix of a pack's first Boss Rush stage, and of every Boss Rush stage. */
constexpr const char *kBossRushFirstFolder = "BR1";
constexpr const char *kBossRushFolderPrefix = "BR";
/** Objects that are checkpoints, by type name without spaces (Sonic 1: lamp posts, Sonic 2: star posts). */
constexpr const char *kCheckpointTypes[] = { "LampPost", "StarPost" };

constexpr const char *kTitleScene = "TITLE SCREEN";
constexpr const char *kCdMenuScene = "MENU";
/** The post-game stage list a cleared save continues at. */
constexpr const char *kStageMenuScene = "STAGE MENU";

/** One of Sonic Origins' modes, as the save screen offers it. */
struct ModeInfo {
    const char *name;
    const char *description;
    int playMode;  ///< game.playMode (the scripts' BOOT_PLAYMODE_* values).
    bool coins;    ///< game.coinMode: extra lives become coins.
    bool mirrored; ///< The port mirrors the picture and swaps left and right.
    bool bossRush; ///< Starts at the first Boss Rush stage, without saving.
};

constexpr ModeInfo kModes[] = {
    { "Anniversary", "Extra lives become coins, and there is no time limit.", 1, true, false, false },
    { "Classic", "The original rules: lives and the 10-minute time limit.", 0, false, false, false },
    // The scripts' own Mirror value (3) pre-flips text for a whole-picture flip;
    // the port mirrors the stage under the HUD and text instead, so it plays
    // with Anniversary's value.
    { "Mirror", "Anniversary rules, with the stages mirrored.", 1, true, true, false },
    { "Boss Rush", "The bosses one after another, without saving.", 2, true, false, true },
};

/** This session's state (the app runs one game per process). */
struct Session {
    const ModeInfo *mode = nullptr; ///< Null until a mode is chosen (and always for packs without modes).
    bool mirrorActive    = false;   ///< Mirror mode and a stage is being played.
    std::string coinsPath;          ///< Where the coin count is kept ("" = not kept).
    std::string gameId;             ///< The retro game being played ("" for Sonic Mania).
    bool achievementsInstalled = false; ///< The game's achievement list replaced Sonic Mania's.
    int coins        = 0;
    bool coinsLoaded = false;       ///< game.coinCount was set from the file.
};

Session &State()
{
    static Session session;
    return session;
}

/** Removes leading and trailing blanks (scene names are padded). */
std::string Trim(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

/** "EMERALD HILL ZONE 2" -> "Emerald Hill Zone 2". */
std::string TitleCase(const std::string &text)
{
    std::string out = text;
    bool wordStart  = true;
    for (char &c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        c                     = static_cast<char>(wordStart ? std::toupper(u) : std::tolower(u));
        wordStart             = c == ' ';
    }
    return out;
}

bool IsNumber(const std::string &text)
{
    if (text.empty())
        return false;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

/** Name of the scene at absolute list position @p pos. */
std::string SceneName(int pos) { return Trim(RSDK::sceneInfo.listData[pos].name); }

/** Absolute list position of scene @p name in @p category, or -1. */
int FindScene(int category, const char *name)
{
    const RSDK::SceneListInfo &list = RSDK::sceneInfo.listCategory[category];
    for (int i = 0; i < list.sceneCount; ++i) {
        if (SceneName(list.sceneOffsetStart + i) == name)
            return list.sceneOffsetStart + i;
    }
    return -1;
}

/**
 * The name of regular stage @p index for people. Later acts are listed by
 * number only ("2"), so the zone name comes from the act before.
 */
std::string RegularStageName(int index)
{
    const RSDK::SceneListInfo &list = RSDK::sceneInfo.listCategory[RSDK::Legacy::STAGELIST_REGULAR];
    if (index < 0 || index >= list.sceneCount)
        return "Unknown stage";

    const std::string name = SceneName(list.sceneOffsetStart + index);
    if (!IsNumber(name))
        return TitleCase(name);
    for (int i = index - 1; i >= 0; --i) {
        std::string zone = SceneName(list.sceneOffsetStart + i);
        if (IsNumber(zone))
            continue;
        // "EMERALD HILL ZONE 1": drop the act number of the first act.
        const std::size_t space = zone.find_last_of(' ');
        if (space != std::string::npos && IsNumber(zone.substr(space + 1)))
            zone = zone.substr(0, space);
        return TitleCase(zone + " " + name);
    }
    return TitleCase(name);
}

int *Slot(int slot) { return &RSDK::Legacy::saveRAM[slot * kSlotValues]; }

bool GlobalExists(const char *name) { return RSDK::Legacy::GetGlobalVariableID(name) != 0xFF; }

/** The characters this game offers: the four of every release, plus those Origins added. */
std::vector<swm::RetroCharacter> Characters()
{
    std::vector<swm::RetroCharacter> characters = { { 0, "Sonic" }, { 1, "Tails" }, { 2, "Knuckles" }, { 3, "Sonic & Tails" } };
    if (GlobalExists("PLAYER_KNUCKLES_TAILS"))
        characters.push_back({ RSDK::Legacy::GetGlobalVariableByName("PLAYER_KNUCKLES_TAILS"), "Knuckles & Tails" });
    if (GlobalExists("PLAYER_AMY"))
        characters.push_back({ RSDK::Legacy::GetGlobalVariableByName("PLAYER_AMY"), "Amy" });
    if (GlobalExists("PLAYER_AMY_TAILS"))
        characters.push_back({ RSDK::Legacy::GetGlobalVariableByName("PLAYER_AMY_TAILS"), "Amy & Tails" });
    return characters;
}

/** True for packs with Sonic Origins' modes. */
bool HasOriginsModes() { return GlobalExists("game.playMode"); }

/** The mode page's entries. */
std::vector<swm::RetroMode> ModeList()
{
    std::vector<swm::RetroMode> modes;
    for (const ModeInfo &mode : kModes)
        modes.push_back(swm::RetroMode{ mode.name, mode.description, !mode.bossRush });
    return modes;
}

/** Sets the globals of mode @p index (see kModes) and remembers it for mirroring. */
void ApplyMode(int index)
{
    const ModeInfo &mode = kModes[std::clamp(index, 0, static_cast<int>(sizeof(kModes) / sizeof(kModes[0])) - 1)];
    RSDK::Legacy::SetGlobalVariableByName("game.playMode", mode.playMode);
    RSDK::Legacy::SetGlobalVariableByName("game.coinMode", mode.coins ? 1 : 0);
    State().mode = &mode;
    std::printf("SonicWebMania: %s mode\n", mode.name);
    std::fflush(stdout);
}

/** Absolute list position of the first Boss Rush stage, or -1. */
int FindBossRushStart()
{
    const RSDK::SceneListInfo &list = RSDK::sceneInfo.listCategory[RSDK::Legacy::STAGELIST_BONUS];
    for (int i = 0; i < list.sceneCount; ++i) {
        const std::string folder = RSDK::sceneInfo.listData[list.sceneOffsetStart + i].folder;
        if (folder.compare(0, std::char_traits<char>::length(kBossRushFirstFolder), kBossRushFirstFolder) == 0)
            return list.sceneOffsetStart + i;
    }
    return -1;
}

/** Starts Boss Rush. @return False if the pack has no Boss Rush stages. */
bool StartBossRush()
{
    const int start = FindBossRushStart();
    if (start < 0)
        return false;
    RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_BONUS;
    RSDK::sceneInfo.listPos        = start;
    return true;
}

/** Key of the coin count in its file ("coins=42"). */
constexpr const char *kCoinsKey = "coins=";

/** Reads the coin count file (0 when missing or unreadable). */
int ReadCoins(const std::string &path)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, std::char_traits<char>::length(kCoinsKey), kCoinsKey) != 0)
            continue;
        try {
            return std::clamp(std::stoi(line.substr(std::char_traits<char>::length(kCoinsKey))), 0, kMaxCoins);
        }
        catch (const std::exception &) {
            return 0;
        }
    }
    return 0;
}

/**
 * Keeps game.coinCount and the coin file in step: the first stage load sets
 * the count from the file, later ones (and FinishRetroSession) write it back
 * when the game changed it.
 */
void SyncCoins()
{
    Session &session = State();
    if (session.coinsPath.empty() || !GlobalExists("game.coinCount"))
        return;

    if (!session.coinsLoaded) {
        session.coins = ReadCoins(session.coinsPath);
        RSDK::Legacy::SetGlobalVariableByName("game.coinCount", session.coins);
        session.coinsLoaded = true;
        return;
    }
    const int now = std::clamp(RSDK::Legacy::GetGlobalVariableByName("game.coinCount"), 0, kMaxCoins);
    if (now == session.coins)
        return;
    session.coins = now;
    std::ofstream out(session.coinsPath, std::ios::trunc);
    out << "; Sonic Origins coins, shared by the retro games (SonicWebMania)\n" << kCoinsKey << now << '\n';
    if (!out)
        std::fprintf(stderr, "SonicWebMania: could not write %s\n", session.coinsPath.c_str());
}

/** Size of Achievements.bin: one 32-bit flag per achievement, as the engine writes it. */
constexpr int kAchievementSlots = 0x100;

/**
 * Replaces Sonic Mania's achievement list (registered by the engine at
 * start-up) with the retro game's, unlocked as the game folder's
 * Achievements.bin says.
 */
void InstallAchievements()
{
    Session &session = State();
    if (session.achievementsInstalled)
        return;
    session.achievementsInstalled = true;

    const std::vector<swm::RetroAchievement> &list = swm::RetroAchievementsFor(session.gameId);
    int32 saved[kAchievementSlots] = {};
    RSDK::SKU::LoadUserFile("Achievements.bin", saved, sizeof(saved));

    RSDK::achievementList.clear();
    for (std::size_t i = 0; i < list.size() && i < static_cast<std::size_t>(kAchievementSlots); ++i) {
        RSDK::AchievementInfo info;
        info.identifier  = list[i].name;
        info.name        = list[i].name;
        info.description = list[i].description;
        info.achieved    = saved[i] != 0;
        RSDK::achievementList.push_back(info);
    }
}

/** Reads the save memory into what the save screen shows. */
swm::SaveSelectRequest BuildRequest()
{
    swm::SaveSelectRequest request;
    request.title      = RSDK::gameVerInfo.gameTitle;
    request.characters = Characters();
    if (HasOriginsModes())
        request.modes = ModeList();

    const RSDK::SceneListInfo &regular = RSDK::sceneInfo.listCategory[RSDK::Legacy::STAGELIST_REGULAR];
    for (int s = 0; s < swm::kSaveSlotCount; ++s) {
        const int *values        = Slot(s);
        swm::RetroSaveSlot &slot = request.slots[static_cast<std::size_t>(s)];
        slot.used                = values[kStage] != 0;
        if (!slot.used)
            continue;

        slot.character = values[kCharacter];
        slot.lives     = values[kLives];
        for (unsigned bits = static_cast<unsigned>(values[kEmeralds]); bits; bits &= bits - 1)
            ++slot.emeralds;

        const int stage = values[kStage];
        if (stage >= kSpecialStageFlag) {
            slot.place = "Special Stage";
        }
        else if (stage - 1 < regular.sceneCount && SceneName(regular.sceneOffsetStart + stage - 1) == kStageMenuScene) {
            slot.place   = "Game cleared";
            slot.cleared = true;
        }
        else {
            slot.place = RegularStageName(stage - 1);
        }
    }
    return request;
}

/** Resets the globals a new game (saved or not) starts from, as the 2013 menu did. */
void SetNewGameGlobals(int saveSlot, bool saving)
{
    using RSDK::Legacy::SetGlobalVariableByName;
    SetGlobalVariableByName("options.attractMode", 0); // A real game, also when the demo was about to start.
    SetGlobalVariableByName("options.saveSlot", saveSlot);
    SetGlobalVariableByName("options.gameMode", saving ? 1 : 0);
    SetGlobalVariableByName("options.stageSelectFlag", 0);
    SetGlobalVariableByName("player.lives", kNewLives);
    SetGlobalVariableByName("player.score", 0);
    SetGlobalVariableByName("player.scoreBonus", kNewScoreBonus);
    SetGlobalVariableByName("specialStage.listPos", 0);
    SetGlobalVariableByName("specialStage.emeralds", 0);
    SetGlobalVariableByName("specialStage.nextZone", 0);
    SetGlobalVariableByName("timeAttack.result", 0);
    SetGlobalVariableByName("lampPostID", 0);
    SetGlobalVariableByName("starPostID", 0);
}

/** Continues @p slot: its globals, character and stage (as the 2013 menu's save select did). */
void ContinueSlot(int slot)
{
    using RSDK::Legacy::SetGlobalVariableByName;
    const int *values = Slot(slot);
    SetGlobalVariableByName("options.attractMode", 0);
    SetGlobalVariableByName("options.saveSlot", slot);
    SetGlobalVariableByName("options.gameMode", 1);
    SetGlobalVariableByName("options.stageSelectFlag", 0);
    SetGlobalVariableByName("player.lives", values[kLives]);
    SetGlobalVariableByName("player.score", values[kScore]);
    SetGlobalVariableByName("player.scoreBonus", values[kScoreBonus]);
    SetGlobalVariableByName("specialStage.listPos", values[kSpecialStage]);
    SetGlobalVariableByName("specialStage.emeralds", values[kEmeralds]);
    SetGlobalVariableByName("lampPostID", 0);
    SetGlobalVariableByName("starPostID", 0);
    RSDK::Legacy::v4::playerListPos = values[kCharacter];

    const int stage = values[kStage];
    if (stage >= kSpecialStageFlag) {
        SetGlobalVariableByName("specialStage.nextZone", stage - (kSpecialStageFlag + 1));
        RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_SPECIAL;
        RSDK::sceneInfo.listPos = RSDK::sceneInfo.listCategory[RSDK::Legacy::STAGELIST_SPECIAL].sceneOffsetStart + values[kSpecialStage];
    }
    else {
        SetGlobalVariableByName("specialStage.nextZone", stage - 1);
        RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_REGULAR;
        RSDK::sceneInfo.listPos        = RSDK::sceneInfo.listCategory[RSDK::Legacy::STAGELIST_REGULAR].sceneOffsetStart + stage - 1;
    }
}

/** Back to the title screen (the save screen was cancelled). */
void GoToTitle()
{
    const int title = FindScene(RSDK::Legacy::STAGELIST_PRESENTATION, kTitleScene);
    RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_PRESENTATION;
    RSDK::sceneInfo.listPos        = title >= 0 ? title : RSDK::sceneInfo.listCategory[0].sceneOffsetStart;
}

/** Sonic 1 and 2: the save screen, then its choice. */
void RunSaveScreen()
{
    RSDK::Legacy::ReadSaveRAM(); // A missing SGame.bin leaves the memory (all slots empty) as it is.
    const swm::SaveSelectResult result = swm::PortRuntime::Instance().RunSaveSelect(BuildRequest());

    bool changed = false;
    for (const int slot : result.deleted) {
        for (int v = 0; v < kSlotValues; ++v)
            Slot(slot)[v] = 0;
        changed = true;
    }

    switch (result.action) {
        case swm::SaveSelectAction::Cancel: GoToTitle(); break;
        case swm::SaveSelectAction::NoSave:
            SetNewGameGlobals(0, false);
            RSDK::Legacy::v4::playerListPos = result.character;
            if (HasOriginsModes() && kModes[result.mode].bossRush && !StartBossRush())
                std::fprintf(stderr, "SonicWebMania: this pack has no Boss Rush stages\n");
            break;
        case swm::SaveSelectAction::NewGame: {
            int *values           = Slot(result.slot);
            values[kCharacter]    = result.character;
            values[kLives]        = kNewLives;
            values[kScore]        = 0;
            values[kScoreBonus]   = kNewScoreBonus;
            values[kStage]        = 1;
            values[kEmeralds]     = 0;
            values[kSpecialStage] = 0;
            changed               = true;
            SetNewGameGlobals(result.slot, true);
            RSDK::Legacy::v4::playerListPos = result.character;
            break;
        }
        case swm::SaveSelectAction::Continue: ContinueSlot(result.slot); break;
    }
    if (result.action != swm::SaveSelectAction::Cancel && HasOriginsModes())
        ApplyMode(result.mode);
    if (changed && !RSDK::Legacy::WriteSaveRAM())
        std::fprintf(stderr, "SonicWebMania: could not write the save file\n");
}

/** Sonic CD: its own 2011 menu, if the pack has it. @return True if the stage was changed. */
bool GoToCdMenu()
{
    const int menu = FindScene(RSDK::Legacy::STAGELIST_PRESENTATION, kCdMenuScene);
    if (menu < 0)
        return false;
    RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_PRESENTATION;
    RSDK::sceneInfo.listPos        = menu;
    return true;
}

/**
 * Sonic CD: with the Origins pack, the mode page first (the 2011 menu has
 * the save slots); Boss Rush starts at its first stage instead of the menu.
 */
void RunCdStart()
{
    if (HasOriginsModes()) {
        swm::SaveSelectRequest request;
        request.title        = RSDK::gameVerInfo.gameTitle;
        request.modes        = ModeList();
        request.slotsEnabled = false; // The 2011 menu has them.
        const swm::SaveSelectResult result = swm::PortRuntime::Instance().RunSaveSelect(request);
        if (result.action == swm::SaveSelectAction::Cancel) {
            GoToTitle();
            return;
        }
        ApplyMode(result.mode);
        if (kModes[result.mode].bossRush) {
            RSDK::Legacy::SetGlobalVariableByName("Options.AttractMode", 0);
            if (StartBossRush())
                return;
            std::fprintf(stderr, "SonicWebMania: this pack has no Boss Rush stages\n");
        }
    }
    GoToCdMenu();
}

} // namespace

extern "C" void WebOSPort_OnLegacyStageLoad(int engineVersion)
{
    // The scene that loaded last, to recognise the step from the title screen.
    static int previousCategory = -1;
    static std::string previousName;

    const bool fromTitle = previousCategory == RSDK::Legacy::STAGELIST_PRESENTATION && previousName == kTitleScene;
    const bool toZone    = RSDK::sceneInfo.activeCategory == RSDK::Legacy::STAGELIST_REGULAR;
    // The demo the title screen plays when left alone also goes to a zone. Only
    // the Origins packs (they have game.playMode) and Sonic CD start games from
    // their title; for the 2013 Sonic 1 and 2 packs the save screen replaces the demo.
    const bool titleStartsGames = engineVersion == 3 || GlobalExists("game.playMode");
    const bool demo             = titleStartsGames
                      && RSDK::Legacy::GetGlobalVariableByName(engineVersion == 3 ? "Options.AttractMode" : "options.attractMode") != 0;

    InstallAchievements();
    // Sonic CD's menu offers its Achievements and Leaderboards entries only
    // with an online service; the port answers them itself.
    if (engineVersion == 3)
        RSDK::Legacy::onlineActive = true;

    if (fromTitle && toZone && !demo) {
        if (engineVersion == 4)
            RunSaveScreen();
        else if (engineVersion == 3)
            RunCdStart();
    }
    SyncCoins();

    previousCategory = RSDK::sceneInfo.activeCategory;
    previousName     = SceneName(RSDK::sceneInfo.listPos);

    // Mirror the stages being played, not the title screen or menus.
    Session &session     = State();
    session.mirrorActive = session.mode && session.mode->mirrored
                           && RSDK::sceneInfo.activeCategory != RSDK::Legacy::STAGELIST_PRESENTATION;
}

extern "C" void WebOSPort_OnLegacyStageLoaded(int engineVersion)
{
    using namespace RSDK::Legacy;
    const Session &session = State();
    if (engineVersion != 4 || !session.mode || !session.mode->bossRush)
        return;
    const std::string folder = RSDK::sceneInfo.listData[RSDK::sceneInfo.listPos].folder;
    if (RSDK::sceneInfo.activeCategory != STAGELIST_BONUS || folder.compare(0, 2, kBossRushFolderPrefix) != 0)
        return;
    if (GetGlobalVariableByName("lampPostID") != 0)
        return; // A retry: the checkpoint already brings the players back.

    // The checkpoint furthest along the stage (the bosses wait at the end),
    // if it is ahead of where the stage starts the player: Sonic 1's Boss Rush
    // stages are whole acts, Sonic 2's already start next to the boss.
    const int32 start = v4::objectEntityList[0].xpos;
    int checkpoint    = -1;
    for (int e = 0; e < LEGACY_v4_TEMPENTITY_START; ++e) {
        const v4::Entity &entity = v4::objectEntityList[e];
        if (!entity.type)
            continue;
        std::string type = v4::typeNames[entity.type];
        type.erase(std::remove(type.begin(), type.end(), ' '), type.end());
        const bool isCheckpoint = std::any_of(std::begin(kCheckpointTypes), std::end(kCheckpointTypes),
                                              [&type](const char *name) { return type == name; });
        if (isCheckpoint && (checkpoint < 0 || entity.xpos > v4::objectEntityList[checkpoint].xpos))
            checkpoint = e;
    }
    std::printf("SonicWebMania: Boss Rush start x %d, last checkpoint x %d\n", start >> 16,
                checkpoint >= 0 ? v4::objectEntityList[checkpoint].xpos >> 16 : -1);
    std::fflush(stdout);
    if (checkpoint < 0 || v4::objectEntityList[checkpoint].xpos <= start)
        return;

    // Load the stage once more, as after a defeat: the checkpoint then places
    // the players and the camera starts there (moving the players now would
    // leave the camera to scroll across the whole act).
    SetGlobalVariableByName("lampPostID", checkpoint);
    SetGlobalVariableByName("starPostID", checkpoint);
    stageMode = STAGEMODE_LOAD;
}

extern "C" int WebOSPort_IsMirrored(void) { return State().mirrorActive ? 1 : 0; }

extern "C" void WebOSPort_OnLegacyAchievement(int id, int status)
{
    InstallAchievements();
    if (id < 0 || id >= static_cast<int>(RSDK::achievementList.size()) || !status)
        return;
    RSDK::AchievementInfo &achievement = RSDK::achievementList[static_cast<std::size_t>(id)];
    if (achievement.achieved)
        return;

    achievement.achieved = true;
    RSDK::SKU::SaveUserData(); // Achievements.bin in the game's folder.
    std::printf("SonicWebMania: achievement unlocked: %s\n", achievement.name.c_str());
    std::fflush(stdout);
    swm::PortRuntime::Instance().Notify("Achievement unlocked: " + achievement.name);
}

extern "C" void WebOSPort_ShowLegacyAchievements(void) { swm::PortRuntime::Instance().ShowAchievements(); }

extern "C" void WebOSPort_ShowLegacyLeaderboards(void)
{
    swm::PortRuntime::Instance().Notify("Leaderboards need an online service; best times are kept in Time Attack");
}

namespace swm
{

void ConfigureRetroSession(const std::string &coinsPath, const std::string &gameId)
{
    State().coinsPath = coinsPath;
    State().gameId    = gameId;
}

void FinishRetroSession()
{
    // Coins collected since the last stage load.
    if (State().coinsLoaded)
        SyncCoins();
}

} // namespace swm
