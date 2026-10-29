/**
 * @file RetroAudio.cpp
 * @brief Picks Sonic Mania stand-ins for a retro game's missing music and
 *        sound effects (see RetroAudio.h). Called by the engine's legacy
 *        audio code through the WebOSPort_ResolveLegacy* hooks.
 */
#include "RSDK/Core/RetroEngine.hpp"
#include "RetroAudio.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace
{

/** A Mania sound effect standing in for a retro one (paths relative to Data/SoundFX/). */
struct SfxStandIn {
    const char *retro;
    const char *mania;
};

/**
 * Retro sound effects Mania has under another name. Those it has under the
 * same name (Global/Jump.wav, Global/DropDash.wav, Global/Grab.wav...) need
 * no entry: the engine finds them in Mania's pack by themselves.
 */
constexpr SfxStandIn kSfxStandIns[] = {
    // Amy's moves (Sonic Origins), which Mania does not have.
    { "Global/HammerJump.wav", "Global/Jump.wav" },
    { "Global/HammerHit.wav", "Stage/Whack.wav" },
    { "Global/HammerDash.wav", "Global/PeelRelease.wav" },
    // Knuckles, and the player's moves.
    { "Global/GlideDropLand.wav", "Global/Land.wav" },
    { "Global/Landing.wav", "Global/Land.wav" },
    { "Global/Rolling.wav", "Global/Roll.wav" },
    { "Global/Transform.wav", "Stage/Transform2.wav" },
    { "Global/LampPost.wav", "Global/StarPost.wav" },
    { "Global/Explosion.wav", "Stage/Explosion.wav" },
    { "Global/BonusPoints.wav", "Global/ScoreAdd.wav" },
    // Menus.
    { "Global/Select.wav", "Global/MenuAccept.wav" },
    { "Global/Resume.wav", "Global/MenuAccept.wav" },
    { "Global/MenuButton.wav", "Global/MenuBleep.wav" },
    { "Global/BackButton.wav", "Global/MenuWoosh.wav" },
    { "Stage/BackButton.wav", "Global/MenuWoosh.wav" },
    { "Global/Event.wav", "Special/Event.wav" },
    { "Stage/Event.wav", "Special/Event.wav" },
    // Sonic CD's time travel.
    { "Global/Future.wav", "SSZ1/Future.wav" },
    { "Global/Past.wav", "SSZ1/Future.wav" },
    { "Global/TimeWarp.wav", "Global/SpecialWarp.wav" },
    // Stage objects.
    { "Stage/AirAlert.wav", "Stage/DrownAlert.wav" },
    { "Stage/Drowning.wav", "Stage/Drown.wav" },
    { "Stage/Breathing.wav", "Stage/Breathe.wav" },
    { "Stage/Spikes.wav", "Global/Spike.wav" },
    { "Stage/Switch.wav", "Stage/Button.wav" },
    { "Stage/ButtonPress.wav", "Stage/Button.wav" },
    { "Stage/BlockBreak.wav", "Stage/LedgeBreak.wav" },
    { "Stage/Crumble.wav", "Stage/LedgeBreak.wav" },
    { "Stage/Bomb.wav", "Stage/Explosion.wav" },
    { "Stage/Emerald.wav", "Special/Emerald.wav" },
    { "Stage/Continue.wav", "Special/Continue.wav" },
    { "Stage/Exit_SS.wav", "Special/SSExit.wav" },
    { "Stage/GrittyGround.wav", "Special/GrittyGround.wav" },
    { "Stage/WaterSplash.wav", "Stage/Splash.wav" },
    { "Stage/WaterGeyser.wav", "HCZ/WaterGush.wav" },
    { "Stage/Launcher.wav", "Stage/Launch.wav" },
    { "Stage/FireBurn.wav", "Stage/Flame.wav" },
    { "Stage/FlameThrower.wav", "Stage/Flame2.wav" },
    { "Stage/SmallFireball.wav", "Stage/Fireball.wav" },
    { "Stage/Saw.wav", "Stage/Buzzsaw.wav" },
    { "Stage/ArrowHit.wav", "PSZ/ArrowHit.wav" },
    { "Stage/GasPop.wav", "OOZ/GasPop.wav" },
    { "Stage/Giggle.wav", "MMZ/Giggle.wav" },
    { "Stage/Elevator.wav", "FBZ/Elevator.wav" },
    { "Stage/Spear.wav", "HCZ/Spear.wav" },
    { "Stage/ChainPull.wav", "HCZ/PullChain.wav" },
    { "Stage/TargetBumper.wav", "Pinball/TargetBumper.wav" },
    { "Stage/Plunger.wav", "Pinball/Plunger.wav" },
    { "Stage/CNZBumper.wav", "Stage/Bumper.wav" },
    { "Stage/MiniBumper.wav", "Stage/Bumper2.wav" },
    { "Stage/Twinkle.wav", "Global/Twinkle.wav" },
    { "Stage/Impact1.wav", "Stage/Impact2.wav" },
    { "Stage/Electricity.wav", "Stage/Electrify.wav" },
    { "Stage/ElectricArc.wav", "Stage/ElecPulse.wav" },
    { "Stage/LaserCharge.wav", "Stage/ElecCharge.wav" },
    { "Stage/GlassSmash.wav", "Stage/WindowShatter.wav" },
    { "Stage/Thruster.wav", "Stage/RocketJet.wav" },
    { "Stage/PushBlock.wav", "Stage/Push.wav" },
    { "Stage/Clank.wav", "Stage/Clang.wav" },
    { "Stage/Door.wav", "Stage/Open.wav" },
    { "Stage/DoorClose.wav", "Stage/HullClose.wav" },
    { "Stage/Shot2.wav", "Stage/Shot.wav" },
    { "Stage/LargeBooster.wav", "Stage/SpeedBooster.wav" },
    { "Stage/SmallBooster.wav", "Stage/SpeedBooster.wav" },
    { "Stage/Crusher.wav", "MMZ/PistonLand.wav" },
    { "Stage/Jab.wav", "Stage/Whack.wav" },
    { "Stage/MechStep.wav", "LRZ/WalkerLegs.wav" },
    // Sonic CD's Metal Sonic.
    { "Stage/MSCharge.wav", "SSZ2/MSChargeFire.wav" },
    { "Stage/MSElectric.wav", "SSZ2/MSElecPulse.wav" },
    { "Stage/MSJet.wav", "SSZ2/SSBoost.wav" },
};

/** Which games a music entry applies to. */
enum Games : unsigned { kSonic1 = 1, kSonic2 = 2, kSonicCd = 4, kAll = kSonic1 | kSonic2 | kSonicCd };

/**
 * A Mania track standing in for a retro one. @c retro is the retro track's
 * name without its folder ("JP/", "US/" for Sonic CD) or extension; @c loop is
 * the sample Mania loops the track at (from its scenes, or its Music object
 * for the jingles), 0 for a track that plays once.
 */
struct MusicStandIn {
    unsigned games;
    const char *retro;
    const char *mania;
    int loop;
    bool similar; ///< Only with "similar music" on: a different zone of similar feel.
};

constexpr MusicStandIn kMusicStandIns[] = {
    // Jingles and the zones Mania remixed.
    { kAll, "Invincibility", "Invincible.ogg", 139263, false },
    { kSonicCd, "SpeedShoes", "Sneakers.ogg", 120960, false },
    { kAll, "ActComplete", "ActClear.ogg", 0, false },
    { kSonicCd, "ZoneComplete", "ActClear.ogg", 0, false },
    { kAll, "Drowning", "Drowning.ogg", 0, false },
    { kAll, "GameOver", "GameOver.ogg", 0, false },
    { kSonic2, "Super", "Super.ogg", 165375, false },
    { kAll, "Boss", "BossEggman1.ogg", 282240, false },
    { kSonic1, "GreenHill", "GreenHill1.ogg", 776160, false },
    { kSonic2, "ChemicalPlant", "ChemicalPlant1.ogg", 680400, false },
    { kSonic2, "OilOcean", "OilOcean1.ogg", 358389, false },
    // Sonic CD: A present, B past, C good future, D bad future.
    { kSonicCd, "R7A", "StardustSpeedway1.ogg", 432894, false },
    { kSonicCd, "R7B", "StardustSpeedway1.ogg", 432894, false },
    { kSonicCd, "R7C", "StardustSpeedway1.ogg", 432894, false },
    { kSonicCd, "R7D", "StardustSpeedway2.ogg", 1, false },
    { kSonicCd, "R8A", "MetallicMadness1.ogg", 470400, false },
    { kSonicCd, "R8B", "MetallicMadness1.ogg", 470400, false },
    { kSonicCd, "R8C", "MetallicMadness1.ogg", 470400, false },
    { kSonicCd, "R8D", "MetallicMadness2.ogg", 296056, false },

    // Similar feel (opt-in).
    { kAll, "TitleScreen", "TitleScreen.ogg", 0, true },
    { kAll, "SpecialStage", "UFOSpecial.ogg", 288655, true },
    { kAll, "Credits", "Credits.ogg", 458752, true },
    { kSonic1, "Final", "BossFinal.ogg", 302400, true },
    { kSonic2 | kSonicCd, "FinalBoss", "BossFinal.ogg", 302400, true },
    { kSonic1, "Marble", "LavaReef1.ogg", 497329, true },
    { kSonic1, "SpringYard", "Studiopolis1.ogg", 1146600, true },
    { kSonic1, "Labyrinth", "Hydrocity1.ogg", 1, true },
    { kSonic1, "Starlight", "StardustSpeedway1.ogg", 432894, true },
    { kSonic1, "ScrapBrain", "MetallicMadness1.ogg", 470400, true },
    { kSonic2, "EmeraldHill", "GreenHill1.ogg", 776160, true },
    { kSonic2, "EmeraldHill2", "GreenHill2.ogg", 775552, true },
    { kSonic2, "AquaticRuin", "Hydrocity1.ogg", 1, true },
    { kSonic2, "CasinoNight", "Studiopolis1.ogg", 1146600, true },
    { kSonic2, "CasinoNight2", "Studiopolis2.ogg", 311142, true },
    { kSonic2, "HillTop", "AngelIsland2.ogg", 160668, true },
    { kSonic2, "MysticCave", "LavaReef1.ogg", 497329, true },
    { kSonic2, "MysticCave2", "LavaReef2.ogg", 769745, true },
    { kSonic2, "Metropolis", "MetallicMadness1.ogg", 470400, true },
    { kSonic2, "SkyChase", "FlyingBattery1.ogg", 158760, true },
    { kSonic2, "WingFortress", "FlyingBattery2.ogg", 70560, true },
    { kSonic2, "DeathEgg", "TitanicMonarch2.ogg", 384873, true },
    { kSonic2, "Options", "MainMenu.ogg", 202752, true },
    { kSonic2, "Results", "Results.ogg", 88991, true },
    { kSonicCd, "R1A", "GreenHill1.ogg", 776160, true }, // Palmtree Panic
    { kSonicCd, "R1B", "GreenHill1.ogg", 776160, true },
    { kSonicCd, "R1C", "GreenHill1.ogg", 776160, true },
    { kSonicCd, "R1D", "GreenHill2.ogg", 775552, true },
    { kSonicCd, "R3A", "Studiopolis1.ogg", 1146600, true }, // Collision Chaos
    { kSonicCd, "R3B", "Studiopolis1.ogg", 1146600, true },
    { kSonicCd, "R3C", "Studiopolis1.ogg", 1146600, true },
    { kSonicCd, "R3D", "Studiopolis2.ogg", 311142, true },
    { kSonicCd, "R4A", "Hydrocity1.ogg", 1, true }, // Tidal Tempest
    { kSonicCd, "R4B", "Hydrocity1.ogg", 1, true },
    { kSonicCd, "R4C", "Hydrocity1.ogg", 1, true },
    { kSonicCd, "R4D", "Hydrocity2.ogg", 329797, true },
    { kSonicCd, "R5A", "LavaReef1.ogg", 497329, true }, // Quartz Quadrant
    { kSonicCd, "R5B", "LavaReef1.ogg", 497329, true },
    { kSonicCd, "R5C", "LavaReef1.ogg", 497329, true },
    { kSonicCd, "R5D", "LavaReef2.ogg", 769745, true },
    { kSonicCd, "R6A", "FlyingBattery1.ogg", 158760, true }, // Wacky Workbench
    { kSonicCd, "R6B", "FlyingBattery1.ogg", 158760, true },
    { kSonicCd, "R6C", "FlyingBattery1.ogg", 158760, true },
    { kSonicCd, "R6D", "FlyingBattery2.ogg", 70560, true },
    { kSonicCd, "TimeAttack", "MainMenu.ogg", 202752, true },
};

/** Mania's speed-shoes jingle, for Sonic 1 and 2's sped-up zone tracks ("..._F.ogg"). */
constexpr MusicStandIn kSpeedShoes = { kSonic1 | kSonic2, "", "Sneakers.ogg", 120960, false };

/** This session's settings (the app runs one game per process). */
struct AudioSession {
    unsigned game     = 0; ///< A Games bit; 0 for Sonic Mania (no stand-ins).
    bool similarMusic = false;
    std::set<std::string> logged; ///< Stand-ins already reported to the log.
};

AudioSession &Session()
{
    static AudioSession session;
    return session;
}

/** Case-insensitive equality, as the engine compares pack paths. */
bool SameName(const std::string &a, const char *b)
{
    const std::size_t length = std::strlen(b);
    if (a.size() != length)
        return false;
    for (std::size_t i = 0; i < length; ++i) {
        const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

/** Where the engine would load @p path from (RSDK::DATAFILE_SOURCE_*). */
int SourceOf(const std::string &path) { return RSDK::FindDataFileSource(path.c_str()); }

/** Copies @p text into the engine's buffer. @return False if it does not fit. */
bool CopyOut(const std::string &text, char *out, int outSize)
{
    if (outSize <= 0 || text.size() >= static_cast<std::size_t>(outSize))
        return false;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return true;
}

/** Reports a stand-in once per name. */
void LogOnce(const std::string &what, const std::string &retro, const std::string &mania)
{
    if (!Session().logged.insert(retro).second)
        return;
    std::printf("SonicWebMania: %s %s from Sonic Mania (%s)\n", what.c_str(), retro.c_str(), mania.empty() ? "silent" : mania.c_str());
    std::fflush(stdout);
}

/** The music entry for retro track @p name of this game, or null. */
const MusicStandIn *FindMusic(const std::string &name)
{
    const AudioSession &session = Session();

    // "JP/R1A.ogg" -> "R1A"; "GreenHill_F.ogg" stays "GreenHill_F".
    std::string base = name;
    const std::size_t slash = base.find_last_of('/');
    if (slash != std::string::npos)
        base = base.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos)
        base = base.substr(0, dot);

    // Sonic 1 and 2 swap in a sped-up copy of the zone's track for speed
    // shoes; Mania plays its speed-shoes jingle instead.
    const bool fast = base.size() > 2 && SameName(base.substr(base.size() - 2), "_F");
    if (fast)
        base = base.substr(0, base.size() - 2);
    if (fast && !SameName(base, "Invincibility"))
        return (kSpeedShoes.games & session.game) ? &kSpeedShoes : nullptr;

    for (const MusicStandIn &entry : kMusicStandIns) {
        if ((entry.games & session.game) && SameName(base, entry.retro) && (!entry.similar || session.similarMusic))
            return &entry;
    }
    return nullptr;
}

} // namespace

namespace swm
{

void ConfigureRetroAudio(const std::string &gameId, bool similarMusic)
{
    AudioSession &session = Session();
    session.game          = gameId == "sonic1" ? kSonic1 : gameId == "sonic2" ? kSonic2 : gameId == "soniccd" ? kSonicCd : 0;
    session.similarMusic  = similarMusic;
}

} // namespace swm

extern "C" int WebOSPort_ResolveLegacySfx(const char *name, char *out, int outSize)
{
    if (!Session().game || !name)
        return 0;
    const std::string retro = name;
    // The game's own packs, or a Mania file of the same name, need nothing.
    if (SourceOf("Data/SoundFX/" + retro) != RSDK::DATAFILE_SOURCE_NONE)
        return 0;
    for (const SfxStandIn &entry : kSfxStandIns) {
        if (SameName(retro, entry.retro) && SourceOf(std::string("Data/SoundFX/") + entry.mania) == RSDK::DATAFILE_SOURCE_AUDIO) {
            LogOnce("sound", retro, entry.mania);
            return CopyOut(entry.mania, out, outSize) ? 1 : 0;
        }
    }
    return 0;
}

extern "C" int WebOSPort_ResolveLegacyMusic(const char *name, char *out, int outSize, int *loopPoint)
{
    if (!Session().game || !name || !*name)
        return 0;
    const std::string retro = name;
    const int source        = SourceOf("Data/Music/" + retro);
    if (source == RSDK::DATAFILE_SOURCE_GAME)
        return 0;

    const MusicStandIn *entry = FindMusic(retro);
    if (entry && SourceOf(std::string("Data/Music/") + entry->mania) == RSDK::DATAFILE_SOURCE_AUDIO) {
        if (!CopyOut(entry->mania, out, outSize))
            return 0;
        if (loopPoint)
            *loopPoint = entry->loop;
        LogOnce("music", retro, entry->mania);
        return 1;
    }
    // A Mania track that only shares the name (its title screen, credits...)
    // is a different song: play nothing rather than that, unless chosen above.
    if (source == RSDK::DATAFILE_SOURCE_AUDIO) {
        LogOnce("music", retro, "");
        return CopyOut("", out, outSize) ? 1 : 0;
    }
    return 0;
}
