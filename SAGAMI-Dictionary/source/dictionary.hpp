#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct DictEntry {
    std::string word;
    std::string reading;
    std::string partOfSpeech;
    std::string gloss;
    uint32_t recordOffset = 0;
};

enum class SearchMode { Japanese, Chinese };

class Dictionary {
public:
    bool open();
    void close();
    bool ready() const { return data_.size() >= 16 && jaIndex_.count > 0; }
    const std::string& error() const { return error_; }
    bool search(const std::string& query, SearchMode mode,
                std::vector<DictEntry>& out, std::size_t limit = 36);

private:
    struct Index {
        std::vector<uint8_t> bytes;
        uint32_t count = 0;
        const uint32_t* offsets = nullptr;
        const uint8_t* blob = nullptr;
    };

    bool loadCompressedFile(const char* path, std::vector<uint8_t>& out);
    bool loadIndex(const char* path, Index& index);
    bool readEntry(uint32_t offset, DictEntry& out);
    static int compareKey(const Index& index, uint32_t item,
                          const std::string& query, bool prefix);
    static const uint8_t* itemPtr(const Index& index, uint32_t item);

    std::vector<uint8_t> data_;
    Index jaIndex_;
    Index zhIndex_;
    std::string error_;
};
