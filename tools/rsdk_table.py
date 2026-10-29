#!/usr/bin/env python3
"""
List and compare the file tables of RSDKv3/v4/vB/v5 data packs (the hashed
"RSDK" format the RSDKv5U engine reads).

Pack layout: "RSDK" 'v' <version byte>, uint16 file count, then per file a
16-byte MD5 of the lower-case path, uint32 offset, uint32 size (top bit set
when the file is encrypted).

    rsdk_table.py A.rsdk               summary of one pack
    rsdk_table.py A.rsdk B.rsdk        files of B that A lacks (what B would add
                                       when loaded after A), with their kinds
"""
import struct
import sys
from typing import Dict, Tuple

Entry = Tuple[int, int, bool]  # offset, size, encrypted


def read_table(path: str) -> Tuple[str, Dict[bytes, Entry]]:
    """Returns the version byte and {md5: (offset, size, encrypted)}."""
    with open(path, "rb") as f:
        header = f.read(6)
        if header[:4] != b"RSDK":
            raise SystemExit(f"{path}: no RSDK signature")
        (count,) = struct.unpack("<H", f.read(2))
        table: Dict[bytes, Entry] = {}
        for _ in range(count):
            md5 = f.read(16)
            offset, size = struct.unpack("<II", f.read(8))
            table[md5] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))
    return chr(header[5]), table


def kind(path: str, entry: Entry) -> str:
    """Guesses a file's type from its first bytes (encrypted files cannot be read)."""
    offset, size, encrypted = entry
    if encrypted:
        return "encrypted"
    with open(path, "rb") as f:
        f.seek(offset)
        head = f.read(4)
    return {b"OggS": "ogg", b"RIFF": "wav"}.get(head, head.hex())


def main() -> None:
    version, a = read_table(sys.argv[1])
    print(f"{sys.argv[1]}: v{version}, {len(a)} files, {sum(e[1] for e in a.values()) / 2**20:.1f} MB,"
          f" {sum(e[2] for e in a.values())} encrypted")
    if len(sys.argv) < 3:
        return
    version_b, b = read_table(sys.argv[2])
    extra = {h: e for h, e in b.items() if h not in a}
    kinds: Dict[str, Tuple[int, int]] = {}
    for e in extra.values():
        k = kind(sys.argv[2], e)
        n, s = kinds.get(k, (0, 0))
        kinds[k] = (n + 1, s + e[1])
    print(f"{sys.argv[2]}: v{version_b}, {len(b)} files; {len(extra)} not in the first pack, by kind:")
    for k, (n, s) in sorted(kinds.items(), key=lambda kv: -kv[1][1])[:12]:
        print(f"  {k:10} {n:5} files {s / 2**20:7.1f} MB")


if __name__ == "__main__":
    main()
