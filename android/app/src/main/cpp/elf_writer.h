#pragma once

#include "xff_linker.h"

#include <cstdint>
#include <string>
#include <vector>

struct SotcImageSection {
    std::string name;
    uint32_t address = 0;
    uint32_t size = 0;
    uint32_t type = 1;
    uint32_t flags = 0;
    uint32_t alignment = 1;
    std::vector<uint8_t> data;
    bool hasData = true;
    bool isCode = false;
};

struct SotcImageSymbol {
    std::string name;
    uint32_t value = 0;
    uint32_t size = 0;
    uint8_t binding = 0;
    uint8_t type = 0;
    uint16_t sectionIndex = 0;
};

struct SotcElfResult {
    uint32_t entry = 0;
    uint32_t flags = 0;
    std::vector<SotcImageSection> sections;
    std::vector<SotcImageSymbol> symbols;
    std::vector<std::string> errors;
};

bool buildSotcElf(const std::vector<uint8_t>& bootElf,
                  const std::vector<XffModule>& modules,
                  const XffLinkResult& linked,
                  SotcElfResult& image,
                  std::string& error);

bool writeSotcElf(const SotcElfResult& image, const std::string& path,
                  std::string& error);

bool writeSotcRecompilerInputs(const SotcElfResult& image,
                               const std::vector<XffModule>& modules,
                               const XffLinkResult& linked,
                               const std::string& outputDirectory,
                               std::string& error);
