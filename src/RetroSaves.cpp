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
 */
#include "RSDK/Core/RetroEngine.hpp"

#include "port/PortRuntime.h"
#include "port/SaveSelect.h"

#include <cctype>
#include <cstdio>
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

constexpr const char *kTitleScene = "TITLE SCREEN";
constexpr const char *kCdMenuScene = "MENU";
/** The post-game stage list a cleared save continues at. */
constexpr const char *kStageMenuScene = "STAGE MENU";

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

/** Reads the save memory into what the save screen shows. */
swm::SaveSelectRequest BuildRequest()
{
    swm::SaveSelectRequest request;
    request.title      = RSDK::gameVerInfo.gameTitle;
    request.characters = Characters();

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
        case swm::SaveSelectAction::Cancel: {
            // Back to the title screen.
            const int title = FindScene(RSDK::Legacy::STAGELIST_PRESENTATION, kTitleScene);
            RSDK::sceneInfo.activeCategory = RSDK::Legacy::STAGELIST_PRESENTATION;
            RSDK::sceneInfo.listPos        = title >= 0 ? title : RSDK::sceneInfo.listCategory[0].sceneOffsetStart;
            break;
        }
        case swm::SaveSelectAction::NoSave:
            SetNewGameGlobals(0, false);
            RSDK::Legacy::v4::playerListPos = result.character;
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

    if (fromTitle && toZone && !demo) {
        if (engineVersion == 4)
            RunSaveScreen();
        else if (engineVersion == 3)
            GoToCdMenu();
    }

    previousCategory = RSDK::sceneInfo.activeCategory;
    previousName     = SceneName(RSDK::sceneInfo.listPos);
}
