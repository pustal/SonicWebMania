#!/usr/bin/env python3
"""
Converts Sonic CD's 2011 Data.rsdk (the original RSDKv3 format, with named
directories and files) into the hashed "RSDKv3" pack the RSDKv5U engine
opens. Reference implementation for the port's on-TV converter
(src/port/RsdkConvert.cpp); both follow RSDKv3-Decompilation's Reader.cpp.

Old format (all integers little-endian):
    u32 headerSize, u16 dirCount
    dirCount x { u8 len, len bytes of name XOR (0xFF - len), u32 offset }
    at headerSize + offset of each directory, its files one after another:
        { u8 len, len bytes of ~name, u32 size, size bytes of data (encrypted) }

New format: "RSDKv3", u16 count, count x { 16-byte MD5 of the lower-case path
(each 4-byte word byte-swapped, as the engine reads them), u32 offset,
u32 size }, then the data, unencrypted.

    rsdk_v3_convert.py OLD.rsdk NEW.rsdk [--list]
"""
import hashlib
import struct
import sys
from typing import List, Tuple

KEY_A = b"4RaS9D7KaEbxcp2o5r6t"
KEY_B = b"3tRaUxLmEaSn"


def decrypt(data: bytes) -> bytes:
    """The old format's file cipher (RSDKv3 FileRead, data-file branch)."""
    size = len(data)
    no = (size & 0x1FC) >> 2
    pos_b = no % 9 + 1
    pos_a = no % pos_b + 1
    swap = False
    out = bytearray(size)
    for i, byte in enumerate(data):
        d = byte ^ KEY_B[pos_b] ^ no
        pos_b += 1
        if swap:
            d = ((d & 0xF) << 4) | (d >> 4)
        d ^= KEY_A[pos_a]
        pos_a += 1
        if pos_a <= 19 or pos_b <= 11:
            if pos_a > 19:
                pos_a = 1
                swap = not swap
            if pos_b > 11:
                pos_b = 1
                swap = not swap
        else:
            no = (no + 1) & 0x7F
            if swap:
                swap = False
                pos_a, pos_b = no % 12 + 6, no % 5 + 4
            else:
                swap = True
                pos_a, pos_b = no % 15 + 3, no % 7 + 1
        out[i] = d
    return bytes(out)


def read_old(path: str) -> List[Tuple[str, bytes]]:
    """Every (path, decrypted data) in an old-format pack."""
    blob = open(path, "rb").read()
    header_size, dir_count = struct.unpack_from("<IH", blob, 0)
    pos = 6
    dirs = []
    for _ in range(dir_count):
        n = blob[pos]
        name = bytes(c ^ (0xFF - n) for c in blob[pos + 1:pos + 1 + n]).decode("latin-1")
        (offset,) = struct.unpack_from("<I", blob, pos + 1 + n)
        dirs.append((name, offset))
        pos += 1 + n + 4
    files = []
    for d, (name, offset) in enumerate(dirs):
        end = (dirs[d + 1][1] if d + 1 < len(dirs) else len(blob) - header_size) + header_size
        pos = header_size + offset
        while pos < end:
            n = blob[pos]
            fname = bytes(~c & 0xFF for c in blob[pos + 1:pos + 1 + n]).decode("latin-1")
            (size,) = struct.unpack_from("<I", blob, pos + 1 + n)
            pos += 1 + n + 4
            files.append((name + fname, decrypt(blob[pos:pos + size])))
            pos += size
    return files


def engine_hash(path: str) -> bytes:
    digest = hashlib.md5(path.lower().encode("latin-1")).digest()
    return b"".join(digest[i:i + 4][::-1] for i in range(0, 16, 4))


def write_new(path: str, files: List[Tuple[str, bytes]]) -> None:
    table_end = 8 + len(files) * 24
    with open(path, "wb") as out:
        out.write(b"RSDKv3" + struct.pack("<H", len(files)))
        offset = table_end
        for name, data in files:
            out.write(engine_hash(name) + struct.pack("<II", offset, len(data)))
            offset += len(data)
        for _, data in files:
            out.write(data)


def main() -> None:
    files = read_old(sys.argv[1])
    if "--list" in sys.argv:
        for name, data in files:
            print(f"{len(data):9} {data[:4]!r:12} {name}")
    write_new(sys.argv[2], files)
    print(f"{len(files)} files, {sum(len(d) for _, d in files) / 2**20:.1f} MB", file=sys.stderr)


if __name__ == "__main__":
    main()
