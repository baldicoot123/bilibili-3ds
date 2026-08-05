#!/usr/bin/env python3
"""Convert Kaikki Japanese JSONL into compact New 3DS dictionary files.

The output deliberately uses a tiny custom format: the 3DS loads both sorted
indexes into RAM and seeks into dict.bin only for selected results.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
from collections import defaultdict
from pathlib import Path

try:
    import jieba  # type: ignore
except ImportError:
    jieba = None

try:
    from opencc import OpenCC  # type: ignore
except ImportError:
    OpenCC = None

try:
    from pypinyin import lazy_pinyin  # type: ignore
except ImportError:
    lazy_pinyin = None

CJK_RE = re.compile(r"[\u3400-\u9fff]{1,16}")
SPLIT_RE = re.compile(r"[，,。；;：:/／、（）()\[\]【】“”‘’!?！？]+")
SPACE_RE = re.compile(r"\s+")


def clean(value: str, limit: int) -> str:
    value = SPACE_RE.sub(" ", value.replace("\x00", " ")).strip()
    encoded = value.encode("utf-8")[:limit]
    while True:
        try:
            return encoded.decode("utf-8")
        except UnicodeDecodeError:
            encoded = encoded[:-1]


def reading_of(obj: dict) -> str:
    for sound in obj.get("sounds", []):
        other = sound.get("other")
        if other and any("ぁ" <= c <= "ヿ" for c in other):
            return clean(other, 240)
    for form in obj.get("forms", []):
        ruby = form.get("ruby")
        if ruby:
            return clean("".join(item[1] for item in ruby if len(item) > 1), 240)
    return ""


def glosses_of(obj: dict) -> list[str]:
    result: list[str] = []
    for sense in obj.get("senses", []):
        for gloss in sense.get("glosses", []):
            gloss = clean(str(gloss), 600)
            if gloss and gloss not in result:
                result.append(gloss)
            if len(result) >= 6:
                return result
    return result


def load_pinyin_dict(path: Path | None) -> tuple[dict[str, str], dict]:
    mapping: dict[str, str] = {}
    trie: dict = {}
    if not path or not path.exists():
        return mapping, trie
    raw = path.read_bytes()
    if len(raw) < 8 or raw[:4] != b"PYD1":
        raise ValueError(f"invalid pinyin dictionary: {path}")
    count = struct.unpack_from("<I", raw, 4)[0]
    table_end = 8 + count * 4
    offsets = struct.unpack_from(f"<{count}I", raw, 8)
    blob = raw[table_end:]
    for offset in offsets:
        py_len, word_len = blob[offset], blob[offset + 1]
        start = offset + 2
        pinyin = blob[start:start + py_len].decode("ascii", "ignore")
        word = blob[start + py_len:start + py_len + word_len].decode("utf-8", "ignore")
        if not word or not pinyin:
            continue
        mapping.setdefault(word, pinyin)
        node = trie
        for char in word:
            node = node.setdefault(char, {})
        node[""] = word
    print(f"loaded {len(mapping):,} pinyin words from {path}")
    return mapping, trie


def trie_terms(text: str, trie: dict) -> set[str]:
    found: set[str] = set()
    for start in range(len(text)):
        node = trie
        for char in text[start:start + 16]:
            node = node.get(char)
            if node is None:
                break
            if "" in node:
                found.add(node[""])
    return found


def reverse_terms(gloss: str, cc, trie: dict) -> set[str]:
    terms: set[str] = set()
    variants = {gloss}
    if cc:
        variants.add(cc.convert(gloss))
    for variant in variants:
        for fragment in SPLIT_RE.split(variant):
            fragment = fragment.strip(" 的地得之指代为是一种用来")
            for cjk in CJK_RE.findall(fragment):
                if len(cjk) <= 12:
                    terms.add(cjk)
                terms.update(trie_terms(cjk, trie))
                if jieba:
                    for token in jieba.cut(cjk, cut_all=False):
                        if 1 <= len(token) <= 12:
                            terms.add(token)
    return terms


def write_index(path: Path, pairs: list[tuple[str, int]]) -> None:
    pairs.sort(key=lambda item: (item[0].encode("utf-8"), item[1]))
    offsets: list[int] = []
    blob = bytearray()
    last = None
    for key, record_offset in pairs:
        key_bytes = key.encode("utf-8")
        if not key_bytes or len(key_bytes) > 65535:
            continue
        marker = (key_bytes, record_offset)
        if marker == last:
            continue
        last = marker
        offsets.append(len(blob))
        blob += struct.pack("<H", len(key_bytes)) + key_bytes + struct.pack("<I", record_offset)
    with path.open("wb") as f:
        f.write(b"SGI1")
        f.write(struct.pack("<II", len(offsets), 0))
        f.write(struct.pack(f"<{len(offsets)}I", *offsets))
        f.write(blob)


def convert(source: Path, output: Path, max_entries: int | None,
            pinyin_dic: Path | None) -> None:
    output.mkdir(parents=True, exist_ok=True)
    cc = OpenCC("t2s") if OpenCC else None
    pinyin_map, pinyin_trie = load_pinyin_dict(pinyin_dic)
    merged: dict[tuple[str, str, str], list[str]] = defaultdict(list)
    with source.open("r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            word = clean(str(obj.get("word", "")), 240)
            reading = reading_of(obj)
            pos = clean(str(obj.get("pos_title") or obj.get("pos") or ""), 120)
            glosses = glosses_of(obj)
            if not word or not glosses:
                continue
            bucket = merged[(word, reading, pos)]
            for gloss in glosses:
                if gloss not in bucket:
                    bucket.append(gloss)
            if max_entries and len(merged) >= max_entries:
                break
            if line_no % 10000 == 0:
                print(f"read {line_no:,} lines; {len(merged):,} merged entries")

    ja_pairs: list[tuple[str, int]] = []
    zh_pairs: list[tuple[str, int]] = []
    dict_path = output / "dict.bin"
    with dict_path.open("wb") as data:
        data.write(b"SGD1" + struct.pack("<III", 1, len(merged), 0))
        for (word, reading, pos), gloss_list in sorted(merged.items()):
            gloss = clean("；".join(gloss_list), 8000)
            values = [word, reading, pos, gloss]
            encoded = [value.encode("utf-8") for value in values]
            if any(len(value) > 65535 for value in encoded):
                continue
            offset = data.tell()
            data.write(struct.pack("<4H", *(len(value) for value in encoded)))
            for value in encoded:
                data.write(value)
            ja_pairs.append((word, offset))
            if reading:
                ja_pairs.append((reading, offset))
            for term in reverse_terms(gloss, cc, pinyin_trie):
                zh_pairs.append((term, offset))
                if term in pinyin_map:
                    zh_pairs.append((pinyin_map[term], offset))
                elif lazy_pinyin:
                    pinyin = "".join(lazy_pinyin(term)).lower()
                    if pinyin:
                        zh_pairs.append((pinyin, offset))

    print(f"writing {len(ja_pairs):,} Japanese index rows")
    write_index(output / "ja.idx", ja_pairs)
    print(f"writing {len(zh_pairs):,} Chinese index rows")
    write_index(output / "zh.idx", zh_pairs)
    for item in (dict_path, output / "ja.idx", output / "zh.idx"):
        print(f"{item.name}: {item.stat().st_size / 1024 / 1024:.2f} MiB")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-entries", type=int)
    parser.add_argument("--pinyin-dic", type=Path)
    args = parser.parse_args()
    convert(args.source, args.output, args.max_entries, args.pinyin_dic)


if __name__ == "__main__":
    main()
