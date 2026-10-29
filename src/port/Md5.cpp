/**
 * @file Md5.cpp
 * @brief MD5 as specified in RFC 1321 (see Md5.h).
 */
#include "Md5.h"

#include <algorithm>
#include <cstring>

namespace swm
{
namespace
{

/** Per-round shift amounts. */
constexpr std::uint32_t kShifts[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };

/** floor(abs(sin(i + 1)) * 2^32). */
constexpr std::uint32_t kSines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

std::uint32_t RotateLeft(std::uint32_t value, std::uint32_t bits) { return (value << bits) | (value >> (32 - bits)); }

} // namespace

Md5::Md5() : state_{ 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 } {}

void Md5::Transform(const std::uint8_t *block)
{
    std::uint32_t words[16];
    for (int i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[i * 4]) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8)
                   | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0, g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        }
        else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        }
        else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t next = d;
        d                        = c;
        c                        = b;
        b                        = b + RotateLeft(a + f + kSines[i] + words[g], kShifts[i]);
        a                        = next;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::Update(const void *data, std::size_t size)
{
    const auto *bytes   = static_cast<const std::uint8_t *>(data);
    std::size_t used    = static_cast<std::size_t>(length_ % 64);
    length_            += size;

    if (used > 0) {
        const std::size_t take = std::min(size, 64 - used);
        std::memcpy(buffer_.data() + used, bytes, take);
        used  += take;
        bytes += take;
        size  -= take;
        if (used < 64)
            return;
        Transform(buffer_.data());
    }
    while (size >= 64) {
        Transform(bytes);
        bytes += 64;
        size  -= 64;
    }
    std::memcpy(buffer_.data(), bytes, size);
}

Md5::Digest Md5::Finish()
{
    const std::uint64_t bits = length_ * 8;
    const std::uint8_t one   = 0x80;
    Update(&one, 1);
    const std::uint8_t zero = 0;
    while (length_ % 64 != 56)
        Update(&zero, 1);

    std::uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    Update(lengthBytes, sizeof(lengthBytes));

    Digest digest{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j)
            digest[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> (8 * j));
    }
    return digest;
}

Md5::Digest Md5::Of(const std::string &text)
{
    Md5 md5;
    md5.Update(text.data(), text.size());
    return md5.Finish();
}

} // namespace swm
