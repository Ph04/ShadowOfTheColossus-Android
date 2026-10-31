#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class IsoSource {
public:
    IsoSource() = default;
    ~IsoSource();

    IsoSource(const IsoSource&) = delete;
    IsoSource& operator=(const IsoSource&) = delete;

    bool attach(int ownedFd, int64_t baseOffset, int64_t length, std::string& error);
    void close();
    bool isOpen() const;
    int64_t size() const;
    bool readAt(uint64_t offset, void* destination, size_t bytes, size_t& bytesRead) const;
    std::string description() const;

private:
    int fd_ = -1;
    int64_t baseOffset_ = 0;
    int64_t length_ = 0;
};
