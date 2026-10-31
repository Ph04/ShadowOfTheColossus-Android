#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    std::string finishHex();

    static std::string hex(const void* data, size_t size);

private:
    void transform(const uint8_t* block);

    uint32_t state_[8];
    uint64_t bitCount_ = 0;
    uint8_t buffer_[64]{};
    size_t buffered_ = 0;
    bool finished_ = false;
};
