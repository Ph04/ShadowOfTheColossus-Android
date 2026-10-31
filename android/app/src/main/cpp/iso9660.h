#pragma once

#include "iso_source.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Iso9660Entry {
    std::string path;
    uint32_t lba = 0;
    uint32_t size = 0;
    bool directory = false;
};

class Iso9660Reader {
public:
    static constexpr uint32_t kSectorSize = 2048;

    explicit Iso9660Reader(const IsoSource& source);
    bool open(std::string& error);
    bool find(std::string_view path, Iso9660Entry& entry) const;
    bool readFile(const Iso9660Entry& entry, std::vector<uint8_t>& output,
                  size_t maximumBytes, std::string& error) const;
    bool hashFile(const Iso9660Entry& entry, std::string& sha256,
                  std::string& error) const;
    std::string volumeId() const;
    uint32_t volumeSectors() const;

private:
    bool walkDirectory(uint32_t lba, uint32_t size, const std::string& prefix,
                       unsigned depth, std::string& error);
    bool rangeValid(uint32_t lba, uint32_t size) const;

    const IsoSource& source_;
    std::unordered_map<std::string, Iso9660Entry> entries_;
    std::string volumeId_;
    uint32_t volumeSectors_ = 0;
    bool opened_ = false;
};
