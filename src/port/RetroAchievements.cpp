/**
 * @file RetroAchievements.cpp
 * @brief The retro games' achievement lists (see RetroAchievements.h).
 */
#include "RetroAchievements.h"

namespace swm
{

const std::vector<RetroAchievement> &RetroAchievementsFor(const std::string &gameId)
{
    static const std::vector<RetroAchievement> kSonic1 = {
        { "Ramp Ring Acrobatics", "Without touching the ground, collect all the rings in a trapezoid formation in Green Hill Zone Act 1" },
        { "Blast Processing", "Clear Green Hill Zone Act 1 in under 30 seconds" },
        { "Secret of Marble Zone", "Travel through a secret room in Marble Zone Act 3" },
        { "Block Buster", "Break 16 blocks in a row without stopping" },
        { "Ring King", "Collect 200 Rings" },
        { "Secret of Labyrinth Zone", "Activate and ride the hidden platform in Labyrinth Zone Act 1" },
        { "Flawless Pursuit", "Clear the boss in Labyrinth Zone without getting hurt" },
        { "Bombs Away", "Defeat the boss in Starlight Zone using only the see-saw bombs" },
        { "Hidden Transporter", "Collect 50 Rings and take the hidden transporter path in Scrap Brain Act 2" },
        { "Chaos Connoisseur", "Collect all the Chaos Emeralds" },
        { "One For the Road", "As a parting gift, land a final hit on Dr. Eggman's escaping Egg Mobile" },
        { "Beat The Clock", "Clear the Time Attack mode in less than 45 minutes" },
    };
    static const std::vector<RetroAchievement> kSonic2 = {
        { "Quick Run", "Complete Emerald Hill Zone Act 1 in under 35 seconds" },
        { "100% Chemical Free", "Complete Chemical Plant without going underwater" },
        { "Early Bird Special", "Collect all the Chaos Emeralds before Chemical Plant" },
        { "Superstar", "Complete any Act as Super Sonic" },
        { "Hit it Big", "Get a jackpot on the Casino Night slot machines" },
        { "Bop Non-stop", "Defeat any boss in 8 consecutive hits without touching the ground" },
        { "Perfectionist", "Get a Perfect Bonus by collecting every Ring in an Act" },
        { "A Secret Revealed", "Find and complete Hidden Palace Zone" },
        { "Head 2 Head", "Win a 2P Versus race against a friend" },
        { "Metropolis Master", "Complete any Metropolis Zone Act without getting hurt" },
        { "Scrambled Egg", "Defeat Dr. Eggman's Boss Attack mode in under 7 minutes" },
        { "Beat the Clock", "Complete the Time Attack mode in less than 45 minutes" },
    };
    static const std::vector<RetroAchievement> kSonicCd = {
        { "88 Miles Per Hour", "Travel through time" },
        { "Just One Hug is Enough", "Get a hug from Amy" },
        { "Paradise Found", "Complete a zone in the Good Future" },
        { "Take the High Road", "Pass the upper Signpost in Collision Chaos Zone 2" },
        { "King of the Rings", "Collect 200 Rings" },
        { "Statue Saviour", "Find the angel statue in Wacky Workbench" },
        { "Heavy Metal", "Defeat Metal Sonic without getting hurt" },
        { "All Stages Clear", "Finish the game" },
        { "Treasure Hunter", "Collect all the Time Stones" },
        { "Dr Eggman Got Served", "Destroy Dr. Eggman's final machine" },
        { "Just In Time", "Complete the Time Attack mode in under 25 minutes" },
        { "Saviour of the Planet", "Destroy all the robot teleporters and Metal Sonic holograms in the past" },
    };
    static const std::vector<RetroAchievement> kNone;

    if (gameId == "sonic1")
        return kSonic1;
    if (gameId == "sonic2")
        return kSonic2;
    if (gameId == "soniccd")
        return kSonicCd;
    return kNone;
}

} // namespace swm
