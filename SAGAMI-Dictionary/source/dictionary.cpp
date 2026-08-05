#include "dictionary.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include <zlib.h>

namespace {
uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
bool readString(const std::vector<uint8_t>& data, std::size_t& cursor,
                uint16_t len, std::string& out) {
    if (cursor > data.size() || len > data.size() - cursor) return false;
    out.resize(len);
    if (len != 0) std::memcpy(out.data(), data.data() + cursor, len);
    cursor += len;
    return true;
}
}

bool Dictionary::open() {
    close();
    if (!loadCompressedFile("romfs:/dict/dict.bin.gz", data_)) {
        return false;
    }
    if (data_.size() < 16 || std::memcmp(data_.data(), "SGD1", 4) != 0) {
        error_ = "词条文件格式不正确";
        close();
        return false;
    }
    if (!loadIndex("romfs:/dict/ja.idx.gz", jaIndex_) ||
        !loadIndex("romfs:/dict/zh.idx.gz", zhIndex_)) {
        close();
        return false;
    }
    error_.clear();
    return true;
}

void Dictionary::close() {
    data_ = {};
    jaIndex_ = {};
    zhIndex_ = {};
}

bool Dictionary::loadCompressedFile(const char* path, std::vector<uint8_t>& out) {
    gzFile f = gzopen(path, "rb");
    if (!f) {
        error_ = std::string("无法打开压缩词库：") + path;
        return false;
    }

    std::vector<uint8_t> bytes;
    std::vector<uint8_t> chunk(64 * 1024);
    for (;;) {
        const int got = gzread(f, chunk.data(), static_cast<unsigned int>(chunk.size()));
        if (got > 0) {
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + got);
            continue;
        }
        if (got < 0) {
            int zlibError = Z_OK;
            const char* message = gzerror(f, &zlibError);
            error_ = std::string("解压词库失败：") + (message ? message : path);
            gzclose(f);
            return false;
        }
        break;
    }
    if (gzclose(f) != Z_OK) {
        error_ = std::string("压缩词库校验失败：") + path;
        return false;
    }
    out = std::move(bytes);
    return true;
}

bool Dictionary::loadIndex(const char* path, Index& index) {
    if (!loadCompressedFile(path, index.bytes)) return false;
    if (index.bytes.size() < 12 || std::memcmp(index.bytes.data(), "SGI1", 4) != 0) {
        error_ = "索引文件格式不正确";
        return false;
    }
    index.count = read32(index.bytes.data() + 4);
    const std::size_t tableEnd = 12u + static_cast<std::size_t>(index.count) * 4u;
    if (tableEnd > index.bytes.size()) {
        error_ = "索引偏移表损坏";
        return false;
    }
    index.offsets = reinterpret_cast<const uint32_t*>(index.bytes.data() + 12);
    index.blob = index.bytes.data() + tableEnd;
    return true;
}

const uint8_t* Dictionary::itemPtr(const Index& index, uint32_t item) {
    if (item >= index.count) return nullptr;
    return index.blob + index.offsets[item];
}

int Dictionary::compareKey(const Index& index, uint32_t item,
                           const std::string& query, bool prefix) {
    const uint8_t* p = itemPtr(index, item);
    if (!p) return 1;
    const uint16_t len = read16(p);
    const char* key = reinterpret_cast<const char*>(p + 2);
    const std::size_t n = std::min<std::size_t>(len, query.size());
    int cmp = std::memcmp(key, query.data(), n);
    if (cmp != 0) return cmp;
    if (prefix && query.size() <= len) return 0;
    if (len < query.size()) return -1;
    if (len > query.size()) return 1;
    return 0;
}

bool Dictionary::readEntry(uint32_t offset, DictEntry& out) {
    if (offset > data_.size() || data_.size() - offset < 8) return false;
    std::size_t cursor = offset;
    const uint8_t* lens = data_.data() + cursor;
    cursor += 8;
    const uint16_t wl = read16(lens + 0);
    const uint16_t rl = read16(lens + 2);
    const uint16_t pl = read16(lens + 4);
    const uint16_t gl = read16(lens + 6);
    if (wl > 2048 || rl > 2048 || pl > 2048 || gl > 8192) return false;
    out.recordOffset = offset;
    return readString(data_, cursor, wl, out.word) &&
           readString(data_, cursor, rl, out.reading) &&
           readString(data_, cursor, pl, out.partOfSpeech) &&
           readString(data_, cursor, gl, out.gloss);
}

bool Dictionary::search(const std::string& query, SearchMode mode,
                        std::vector<DictEntry>& out, std::size_t limit) {
    out.clear();
    if (!ready() || query.empty()) return false;
    const Index& index = mode == SearchMode::Japanese ? jaIndex_ : zhIndex_;
    uint32_t lo = 0, hi = index.count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (compareKey(index, mid, query, false) < 0) lo = mid + 1;
        else hi = mid;
    }
    std::set<uint32_t> seen;
    for (uint32_t i = lo; i < index.count && out.size() < limit; ++i) {
        if (compareKey(index, i, query, true) != 0) break;
        const uint8_t* p = itemPtr(index, i);
        const uint16_t keyLen = read16(p);
        const uint32_t recordOffset = read32(p + 2 + keyLen);
        if (!seen.insert(recordOffset).second) continue;
        DictEntry entry;
        if (readEntry(recordOffset, entry)) out.push_back(std::move(entry));
    }
    return !out.empty();
}
