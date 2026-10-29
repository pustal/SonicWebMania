/**
 * @file RetroAudio.h
 * @brief Sonic Mania's music and sound effects standing in for those a retro
 *        game's packs lack (src/RetroAudio.cpp).
 *
 * The Sonic Origins packs have no audio, and even with the earlier releases'
 * packs the sounds Origins added (the Drop Dash, Knuckles' and Amy's moves)
 * are missing. Sonic Mania's pack has many of them under the same names, and
 * remixes of several of the retro zones' music. When Mania's pack is
 * installed, RetroLibrary::PrepareLaunch() loads it as the engine's audio
 * pack (patches/rsdk/0006) and the engine asks this module, for each sound
 * effect and track the game's packs lack, what to play instead:
 *   - sound effects: Mania's file of the same name, or its nearest one
 *     (Amy's hammer, for instance, uses Mania's hits and jumps);
 *   - music: the Mania remix of the same zone or jingle (Green Hill, Chemical
 *     Plant, Stardust Speedway, invincibility...), with Mania's loop point;
 *   - with "similar music" on (off by default), zones Mania has no version of
 *     get a track of similar feel (Emerald Hill plays Green Hill, Casino
 *     Night plays Studiopolis...).
 * Whatever the game's own packs have always wins.
 */
#ifndef SWM_RETRO_AUDIO_H
#define SWM_RETRO_AUDIO_H

#include <string>

namespace swm
{

/**
 * Sets up the stand-ins for this session, before the engine starts.
 *
 * @param gameId       The retro game about to run ("sonic1", "sonic2",
 *                     "soniccd"); anything else (Sonic Mania) uses none.
 * @param similarMusic Also give zones Mania has no version of a track of
 *                     similar feel.
 */
void ConfigureRetroAudio(const std::string &gameId, bool similarMusic);

} // namespace swm

#endif // SWM_RETRO_AUDIO_H
