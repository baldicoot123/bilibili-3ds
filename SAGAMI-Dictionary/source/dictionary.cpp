#include "dictionary.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace {
uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t read32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
bool readString(FILE* f, uint16_t len, std::string& out) {
    out.resize(len);
    return len == 0 || std::fread(out.data(), 1, len, f) == len;
}
}

bool Dictionary::open() {
    close();
    dataFile_ = std::fopen("romfs:/dict/dict.bin", "rb");
    if (!dataFile_) {
        error_ = "无法打开 romfs:/dict/dict.bin";
        return false;
    }
    uint8_t header[16]{};
    if (std::fread(header, 1, sizeof(header), dataFile_) != sizeof(header) ||
        std::memcmp(header, "SGD1", 4) != 0) {
        error_ = "词条文件格式不正确";
        close();
        return false;
    }
    if (!loadIndex("romfs:/dict/ja.idx", jaIndex_) ||
        !loadIndex("romfs:/dict/zh.idx", zhIndex_)) {
        close();
        return false;
    }
    error_.clear();
    return true;
}

void Dictionary::close() {
    if (dataFile_) std::fclose(dataFile_);
    dataFile_ = nullptr;
    jaIndex_ = {};
    zhIndex_ = {};
}

bool Dictionary::loadIndex(const char* path, Index& index) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        error_ = std::string("无法打开索引：") + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 12) {
        std::fclose(f);
        error_ = "索引文件过小";
        return false;
    }
    index.bytes.resize(static_cast<std::size_t>(size));
    bool ok = std::fread(index.bytes.data(), 1, index.bytes.size(), f) == index.bytes.size();
    std::fclose(f);
    if (!ok || std::memcmp(index.bytes.data(), "SGI1", 4) != 0) {
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
    if (!dataFile_ || std::fseek(dataFile_, static_cast<long>(offset), SEEK_SET) != 0)
        return false;
    uint8_t lens[8];
    if (std::fread(lens, 1, sizeof(lens), dataFile_) != sizeof(lens)) return false;
    const uint16_t wl = read16(lens + 0);
    const uint16_t rl = read16(lens + 2);
    const uint16_t pl = read16(lens + 4);
    const uint16_t gl = read16(lens + 6);
    if (wl > 2048 || rl > 2048 || pl > 2048 || gl > 8192) return false;
    out.recordOffset = offset;
    return readString(dataFile_, wl, out.word) &&
           readString(dataFile_, rl, out.reading) &&
           readString(dataFile_, pl, out.partOfSpeech) &&
           readString(dataFile_, gl, out.gloss);
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
