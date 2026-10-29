/**
 * @file Md5.h
 * @brief MD5 (RFC 1321), for the file-name hashes RSDK data packs index by.
 *
 * Not used for anything security-related: RSDK packs look files up by the
 * MD5 of their lower-case path, so the port needs the same hash to look into
 * a pack or to write one.
 */
#ifndef SWM_MD5_H
#define SWM_MD5_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace swm
{

/** Incremental MD5. */
class Md5
{
public:
    using Digest = std::array<std::uint8_t, 16>;

    Md5();

    /** Adds @p size bytes at @p data. */
    void Update(const void *data, std::size_t size);

    /** Finishes and returns the 16-byte digest. The object must not be updated afterwards. */
    Digest Finish();

    /** Digest of @p text. */
    static Digest Of(const std::string &text);

private:
    /** Processes one 64-byte block. */
    void Transform(const std::uint8_t *block);

    std::array<std::uint32_t, 4> state_;
    std::uint64_t length_ = 0; ///< Bytes added so far.
    std::array<std::uint8_t, 64> buffer_{};
};

} // namespace swm

#endif // SWM_MD5_H
