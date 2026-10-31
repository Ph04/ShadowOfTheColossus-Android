#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct XffSection {
    uint32_t index = 0;
    std::string name;
    uint32_t runtimeAddress = 0;
    uint32_t imageOffset = 0;
    uint32_t size = 0;
    uint32_t alignment = 1;
    uint32_t type = 0;
    uint32_t flags = 0;
    uint32_t fileOffset = 0;
    bool hasImage = false;
};

struct XffSymbol {
    uint32_t index = 0;
    std::string name;
    uint32_t value = 0;
    uint32_t size = 0;
    uint8_t binding = 0;
    uint8_t type = 0;
    uint16_t sectionIndex = 0;
};

struct XffRelocation {
    uint32_t offset = 0;
    uint32_t symbolIndex = 0;
    uint32_t type = 0;
    uint32_t originalWord = 0;
    uint32_t auxiliaryWord = 0;
};

struct XffRelocationTable {
    uint32_t targetSection = 0;
    std::vector<XffRelocation> entries;
};

struct XffModule {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t entryOffset = 0;
    std::vector<XffSection> sections;
    std::vector<XffSymbol> symbols;
    std::vector<XffRelocationTable> relocationTables;
};

struct XffLinkedModule {
    size_t moduleIndex = 0;
    uint32_t base = 0;
    uint32_t bssBase = 0;
    uint32_t entryAddress = 0;
    std::vector<uint8_t> image;
    std::map<uint32_t, uint32_t> sectionAddresses;
    std::map<uint32_t, uint32_t> symbolAddresses;
    size_t unresolvedImportCount = 0;
};

struct XffUnresolvedImport {
    std::string module;
    std::string section;
    uint32_t address = 0;
    uint32_t relocationType = 0;
    std::string symbol;
    uint32_t originalWord = 0;
    uint32_t linkTimeWord = 0;
};

struct XffDuplicateExport {
    std::string name;
    uint32_t previous = 0;
    uint32_t selected = 0;
    std::string module;
};

struct XffLinkResult {
    std::vector<XffLinkedModule> modules;
    std::map<std::string, uint32_t> exports;
    std::vector<XffUnresolvedImport> unresolvedImports;
    std::vector<XffDuplicateExport> duplicateExports;
};

bool parseXffModule(const std::string& name, const std::vector<uint8_t>& bytes,
                    XffModule& output, std::string& error);
bool linkXffModules(const std::vector<XffModule>& modules,
                    const std::vector<uint32_t>& loadBases,
                    const std::vector<uint32_t>& bssBases,
                    uint32_t undefinedStub,
                    XffLinkResult& result,
                    std::string& error);
