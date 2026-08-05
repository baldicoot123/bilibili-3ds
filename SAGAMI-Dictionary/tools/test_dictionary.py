#!/usr/bin/env python3
from __future__ import annotations

import bisect
import struct
import sys
from pathlib import Path


class Index:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        assert raw[:4] == b"SGI1"
        self.count = struct.unpack_from("<I", raw, 4)[0]
        self.offsets = struct.unpack_from(f"<{self.count}I", raw, 12)
        self.blob = raw[12 + self.count * 4:]
        self.keys: list[bytes] = []
        self.records: list[int] = []
        for offset in self.offsets:
            key_len = struct.unpack_from("<H", self.blob, offset)[0]
            start = offset + 2
            self.keys.append(self.blob[start:start + key_len])
            self.records.append(struct.unpack_from("<I", self.blob, start + key_len)[0])

    def search(self, query: str, limit: int = 8) -> list[int]:
        needle = query.encode("utf-8")
        at = bisect.bisect_left(self.keys, needle)
        found: list[int] = []
        for i in range(at, len(self.keys)):
            if not self.keys[i].startswith(needle):
                break
            if self.records[i] not in found:
                found.append(self.records[i])
            if len(found) >= limit:
                break
        return found


def read_entry(data, offset: int) -> tuple[str, str, str, str]:
    data.seek(offset)
    lengths = struct.unpack("<4H", data.read(8))
    return tuple(data.read(length).decode("utf-8") for length in lengths)  # type: ignore


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    root = Path(__file__).resolve().parents[1] / "romfs" / "dict"
    ja = Index(root / "ja.idx")
    zh = Index(root / "zh.idx")
    assert ja.keys == sorted(ja.keys)
    assert zh.keys == sorted(zh.keys)
    with (root / "dict.bin").open("rb") as data:
        assert data.read(4) == b"SGD1"
        for index, query in ((ja, "日本"), (ja, "にほん"), (zh, "学习"),
                             (zh, "xuexi"), (zh, "riben")):
            records = index.search(query)
            print(f"{query}: {len(records)} results")
            for record in records[:3]:
                print("  ", read_entry(data, record))
            assert records, f"no result for {query}"


if __name__ == "__main__":
    main()
