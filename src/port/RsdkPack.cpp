/**
 * @file RsdkPack.cpp
 * @brief Pack inspection and the Sonic CD 2011 pack converter (see RsdkPack.h).
 */
#include "RsdkPack.h"

#include "Md5.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace swm
{
namespace RsdkPack
{
namespace
{

/** Bytes of a hashed pack's header and of one table entry. */
constexpr std::size_t kHashedHeaderBytes = 8;
constexpr std::size_t kHashedEntryBytes  = 24;
/** Sanity limits for the original format's header. */
constexpr std::uint32_t kMaxOldDirectories = 4096;
constexpr std::size_t kMaxFiles            = 0xFFFF; // The hashed table's count is 16 bits.
/** Copy buffer for the conversion. */
constexpr std::size_t kChunkBytes = 64 * 1024;

/** Cipher keys of the original RSDKv3 format. */
constexpr char kOldKeyA[] = "4RaS9D7KaEbxcp2o5r6t";
constexpr char kOldKeyB[] = "3tRaUxLmEaSn";

/** An fopen()ed file closed on scope exit. */
using File = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;

File Open(const std::string &path, const char *mode) { return File(std::fopen(path.c_str(), mode), &std::fclose); }

bool ReadU8(std::FILE *file, std::uint8_t *out) { return std::fread(out, 1, 1, file) == 1; }

bool ReadU16(std::FILE *file, std::uint16_t *out)
{
    std::uint8_t b[2];
    if (std::fread(b, 1, 2, file) != 2)
        return false;
    *out = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool ReadU32(std::FILE *file, std::uint32_t *out)
{
    std::uint8_t b[4];
    if (std::fread(b, 1, 4, file) != 4)
        return false;
    *out = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) | (static_cast<std::uint32_t>(b[2]) << 16)
           | (static_cast<std::uint32_t>(b[3]) << 24);
    return true;
}

void PutU32(std::uint8_t *out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

/** The 16 table bytes the engine compares for @p path. */
std::array<std::uint8_t, 16> PathKey(const std::string &path)
{
    std::string lower = path;
    for (char &c : lower)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    const Md5::Digest digest = Md5::Of(lower);

    std::array<std::uint8_t, 16> key{};
    for (std::size_t word = 0; word < 4; ++word) {
        for (std::size_t b = 0; b < 4; ++b)
            key[word * 4 + b] = digest[word * 4 + (3 - b)];
    }
    return key;
}

std::int64_t FileSize(std::FILE *file)
{
    if (std::fseek(file, 0, SEEK_END) != 0)
        return -1;
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    return size;
}

/** The original format's per-file cipher (RSDKv3 FileRead, data-file branch); stateful across chunks. */
class OldV3Cipher
{
public:
    explicit OldV3Cipher(std::uint32_t fileSize)
        : number_(static_cast<int>((fileSize & 0x1FC) >> 2)), posB_(number_ % 9 + 1), posA_(number_ % posB_ + 1)
    {
    }

    void Apply(std::uint8_t *data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i) {
            std::uint8_t d = static_cast<std::uint8_t>(data[i] ^ static_cast<std::uint8_t>(kOldKeyB[posB_++]) ^ number_);
            if (swap_)
                d = static_cast<std::uint8_t>(((d & 0xF) << 4) | (d >> 4));
            d ^= static_cast<std::uint8_t>(kOldKeyA[posA_++]);
            data[i] = d;

            if (posA_ <= 19 || posB_ <= 11) {
                if (posA_ > 19) {
                    posA_ = 1;
                    swap_ = !swap_;
                }
                if (posB_ > 11) {
                    posB_ = 1;
                    swap_ = !swap_;
                }
            }
            else {
                number_ = (number_ + 1) & 0x7F;
                if (swap_) {
                    swap_ = false;
                    posA_ = number_ % 12 + 6;
                    posB_ = number_ % 5 + 4;
                }
                else {
                    swap_ = true;
                    posA_ = number_ % 15 + 3;
                    posB_ = number_ % 7 + 1;
                }
            }
        }
    }

private:
    int number_;
    int posB_;
    int posA_;
    bool swap_ = false;
};

/** A directory of the original format. */
struct OldDirectory {
    std::string name;     ///< With its trailing '/', e.g. "Data/Game/".
    std::uint32_t offset; ///< From the end of the header.
};

/** A file of the original format. */
struct OldFile {
    std::string path;        ///< Directory plus file name.
    std::uint32_t dataStart; ///< Absolute offset of the (encrypted) data.
    std::uint32_t size;
};

/** Reads a directory name (XOR 0xFF - length). */
bool ReadDirectoryName(std::FILE *file, std::string *name)
{
    std::uint8_t length = 0;
    if (!ReadU8(file, &length))
        return false;
    name->resize(length);
    if (length && std::fread(&(*name)[0], 1, length, file) != length)
        return false;
    for (char &c : *name)
        c = static_cast<char>(static_cast<std::uint8_t>(c) ^ static_cast<std::uint8_t>(0xFF - length));
    return true;
}

/** Reads the original format's header. */
bool ReadOldHeader(std::FILE *file, std::int64_t fileSize, std::uint32_t *headerSize, std::vector<OldDirectory> *dirs)
{
    std::uint16_t dirCount = 0;
    if (!ReadU32(file, headerSize) || !ReadU16(file, &dirCount))
        return false;
    if (*headerSize < 6 || *headerSize > fileSize || dirCount == 0 || dirCount > kMaxOldDirectories)
        return false;

    dirs->clear();
    for (std::uint16_t d = 0; d < dirCount; ++d) {
        OldDirectory dir;
        if (!ReadDirectoryName(file, &dir.name) || !ReadU32(file, &dir.offset))
            return false;
        if (static_cast<std::int64_t>(*headerSize) + dir.offset > fileSize)
            return false;
        dirs->push_back(dir);
    }
    return true;
}

} // namespace

bool Contains(const std::string &packPath, const std::string &filePath)
{
    File file = Open(packPath, "rb");
    std::uint8_t header[kHashedHeaderBytes];
    if (!file || std::fread(header, 1, sizeof(header), file.get()) != sizeof(header) || std::memcmp(header, "RSDK", 4) != 0)
        return false;

    const std::size_t count               = static_cast<std::size_t>(header[6] | (header[7] << 8));
    const std::array<std::uint8_t, 16> key = PathKey(filePath);
    std::uint8_t entry[kHashedEntryBytes];
    for (std::size_t i = 0; i < count; ++i) {
        if (std::fread(entry, 1, sizeof(entry), file.get()) != sizeof(entry))
            return false;
        if (std::memcmp(entry, key.data(), key.size()) == 0)
            return true;
    }
    return false;
}

bool HasEngineBytecode(const std::string &packPath)
{
    return Contains(packPath, "Bytecode/GlobalCode.bin") || Contains(packPath, "Data/Scripts/ByteCode/GlobalCode.bin");
}

bool LooksLikeOldV3Pack(const std::string &path)
{
    File file = Open(path, "rb");
    if (!file)
        return false;
    const std::int64_t size = FileSize(file.get());
    std::uint32_t headerSize = 0;
    std::uint16_t dirCount   = 0;
    std::string firstName;
    if (!ReadU32(file.get(), &headerSize) || !ReadU16(file.get(), &dirCount) || !ReadDirectoryName(file.get(), &firstName))
        return false;
    return headerSize >= 6 && headerSize <= size && dirCount > 0 && dirCount <= kMaxOldDirectories
           && firstName.compare(0, 5, "Data/") == 0;
}

bool ConvertOldV3Pack(const std::string &inPath, const std::string &outPath, std::string *error)
{
    auto fail = [&](const std::string &reason) {
        if (error)
            *error = reason;
        std::remove(outPath.c_str());
        return false;
    };

    File in = Open(inPath, "rb");
    if (!in)
        return fail("cannot open the pack");
    const std::int64_t inSize = FileSize(in.get());

    std::uint32_t headerSize = 0;
    std::vector<OldDirectory> dirs;
    if (!ReadOldHeader(in.get(), inSize, &headerSize, &dirs))
        return fail("not a Sonic CD 2011 pack (unreadable header)");

    // Walk each directory's files: { u8 length, ~name, u32 size, data }.
    std::vector<OldFile> files;
    for (std::size_t d = 0; d < dirs.size(); ++d) {
        const std::int64_t end = static_cast<std::int64_t>(headerSize)
                                 + (d + 1 < dirs.size() ? dirs[d + 1].offset : static_cast<std::uint32_t>(inSize - headerSize));
        std::int64_t pos = static_cast<std::int64_t>(headerSize) + dirs[d].offset;
        while (pos < end) {
            if (std::fseek(in.get(), static_cast<long>(pos), SEEK_SET) != 0)
                return fail("damaged pack (bad file position)");
            std::uint8_t length = 0;
            std::string name;
            std::uint32_t size = 0;
            if (!ReadU8(in.get(), &length))
                return fail("damaged pack (truncated file entry)");
            name.resize(length);
            if (length && std::fread(&name[0], 1, length, in.get()) != length)
                return fail("damaged pack (truncated file name)");
            for (char &c : name)
                c = static_cast<char>(~static_cast<std::uint8_t>(c));
            if (!ReadU32(in.get(), &size))
                return fail("damaged pack (truncated file size)");

            const std::int64_t dataStart = pos + 1 + length + 4;
            if (dataStart + size > end)
                return fail("damaged pack (file runs past its directory)");
            if (files.size() >= kMaxFiles)
                return fail("too many files for one pack");
            files.push_back(OldFile{ dirs[d].name + name, static_cast<std::uint32_t>(dataStart), size });
            pos = dataStart + size;
        }
    }
    if (files.empty())
        return fail("the pack has no files");

    File out = Open(outPath, "wb");
    if (!out)
        return fail("cannot create the converted pack");

    // Header and table first; the data follows in the same order.
    std::vector<std::uint8_t> table(kHashedHeaderBytes + files.size() * kHashedEntryBytes);
    std::memcpy(table.data(), "RSDKv3", 6);
    table[6] = static_cast<std::uint8_t>(files.size() & 0xFF);
    table[7] = static_cast<std::uint8_t>(files.size() >> 8);
    std::uint64_t offset = table.size();
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::uint8_t *entry                    = table.data() + kHashedHeaderBytes + i * kHashedEntryBytes;
        const std::array<std::uint8_t, 16> key = PathKey(files[i].path);
        std::memcpy(entry, key.data(), key.size());
        if (offset > 0xFFFFFFFFull)
            return fail("the converted pack would be too big");
        PutU32(entry + 16, static_cast<std::uint32_t>(offset));
        PutU32(entry + 20, files[i].size & 0x7FFFFFFF); // Unencrypted.
        offset += files[i].size;
    }
    if (std::fwrite(table.data(), 1, table.size(), out.get()) != table.size())
        return fail("cannot write the converted pack (disk full?)");

    std::vector<std::uint8_t> chunk(kChunkBytes);
    for (const OldFile &file : files) {
        if (std::fseek(in.get(), static_cast<long>(file.dataStart), SEEK_SET) != 0)
            return fail("damaged pack (bad data position)");
        OldV3Cipher cipher(file.size);
        std::uint32_t remaining = file.size;
        while (remaining > 0) {
            const std::size_t take = std::min<std::size_t>(remaining, chunk.size());
            if (std::fread(chunk.data(), 1, take, in.get()) != take)
                return fail("damaged pack (truncated data)");
            cipher.Apply(chunk.data(), take);
            if (std::fwrite(chunk.data(), 1, take, out.get()) != take)
                return fail("cannot write the converted pack (disk full?)");
            remaining -= static_cast<std::uint32_t>(take);
        }
    }

    if (std::fflush(out.get()) != 0 || fsync(fileno(out.get())) != 0)
        return fail("cannot write the converted pack (disk full?)");
    return true;
}

} // namespace RsdkPack
} // namespace swm
