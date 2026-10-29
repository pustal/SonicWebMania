/**
 * @file RsdkPack.h
 * @brief Looking inside RSDK data packs, and converting Sonic CD's 2011 pack
 *        into the format the engine opens.
 *
 * The engine (RSDKv5U) opens "hashed" packs: the signature "RSDK", 'v' and a
 * version byte ('3', '4', 'B' or '5'), a uint16 file count, then for each
 * file the MD5 of its lower-case path (each 4-byte word byte-swapped, the
 * order the engine reads it in), a uint32 offset and a uint32 size whose top
 * bit marks encryption.
 *
 * Sonic CD's 2011 release (Steam and others) uses the original RSDKv3
 * format instead, which the engine cannot open: named directories and files
 * and a fixed-key cipher (see RSDKv3-Decompilation's Reader.cpp, and
 * tools/rsdk_v3_convert.py for the same converter in Python).
 * ConvertOldV3Pack() rewrites such a pack as a hashed, unencrypted "RSDKv3"
 * pack. The mobile release's pack (Data.rsdk.xmf) then plays on its own; the
 * Steam release's scripts are stored under the PC build's names, which the
 * engine does not load, but its music and sound effects complete the Origins
 * game.
 */
#ifndef SWM_RSDK_PACK_H
#define SWM_RSDK_PACK_H

#include <string>

namespace swm
{
namespace RsdkPack
{

/**
 * True when the hashed pack at @p packPath lists @p filePath (compared
 * without regard to case, as the engine does).
 */
bool Contains(const std::string &packPath, const std::string &filePath);

/**
 * True when the hashed pack has the bytecode the engine's legacy cores run
 * ("Bytecode/GlobalCode.bin" for RSDKv4, "Data/Scripts/ByteCode/GlobalCode.bin"
 * for RSDKv3), i.e. it can be played on its own rather than only lend its
 * sound to an Origins pack.
 */
bool HasEngineBytecode(const std::string &packPath);

/**
 * Quick check that @p path starts like an original-format RSDKv3 pack: a
 * plausible header size and directory count, and a first directory whose
 * name decodes to "Data/...".
 */
bool LooksLikeOldV3Pack(const std::string &path);

/**
 * Converts the original-format RSDKv3 pack at @p inPath into a hashed
 * "RSDKv3" pack at @p outPath (files decrypted, offsets recomputed). Streams
 * through the files, so memory use stays small; the output is about the size
 * of the input.
 *
 * @return False with a reason in @p error if the input is malformed or the
 *         output cannot be written (a partial output is removed).
 */
bool ConvertOldV3Pack(const std::string &inPath, const std::string &outPath, std::string *error);

} // namespace RsdkPack
} // namespace swm

#endif // SWM_RSDK_PACK_H
