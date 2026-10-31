#include "xff_linker.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>

namespace {
constexpr uint32_t kMagicBias = 0x40010000u;
constexpr uint32_t kHeaderSize = 0x70u;
constexpr uint32_t kShtProgbits = 1u;
constexpr uint32_t kShtRel = 9u;
constexpr uint32_t kShtNobits = 8u;
constexpr uint32_t kShfAlloc = 0x2u;
constexpr uint32_t kShnUndef = 0u;
constexpr uint32_t kShnAbs = 0xFFF1u;
constexpr uint32_t kShnCommon = 0xFFF2u;
constexpr uint32_t kStbLocal = 0u;

bool rangeOk(size_t total, uint64_t offset, uint64_t length) {
    return offset <= total && length <= static_cast<uint64_t>(total) - offset;
}

bool readU32(const std::vector<uint8_t>& data, uint64_t offset, uint32_t& value,
             const std::string& label, std::string& error) {
    if (!rangeOk(data.size(), offset, 4)) {
        error = label + ": leitura fora dos limites do arquivo.";
        return false;
    }
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8u) |
            (static_cast<uint32_t>(data[offset + 2]) << 16u) |
            (static_cast<uint32_t>(data[offset + 3]) << 24u);
    return true;
}

bool readU16(const std::vector<uint8_t>& data, uint64_t offset, uint16_t& value,
             const std::string& label, std::string& error) {
    if (!rangeOk(data.size(), offset, 2)) {
        error = label + ": leitura fora dos limites do arquivo.";
        return false;
    }
    value = static_cast<uint16_t>(data[offset] |
            (static_cast<uint16_t>(data[offset + 1]) << 8u));
    return true;
}

bool readString(const std::vector<uint8_t>& data, uint64_t offset,
                std::string& value, const std::string& label, std::string& error) {
    if (offset >= data.size()) {
        error = label + ": início da string fora do arquivo.";
        return false;
    }
    const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = std::find(begin, data.end(), 0);
    if (end == data.end()) {
        error = label + ": string sem terminador nulo.";
        return false;
    }
    value.assign(begin, end);
    return true;
}

bool checkedArray(const std::vector<uint8_t>& data, uint32_t offset,
                  uint32_t count, uint32_t stride, const std::string& label,
                  std::string& error) {
    if (stride == 0 || count > data.size() / stride ||
        !rangeOk(data.size(), offset, static_cast<uint64_t>(count) * stride)) {
        error = label + ": tabela fora dos limites.";
        return false;
    }
    return true;
}

uint32_t alignValue(uint32_t value, uint32_t alignment) {
    alignment = std::max(1u, alignment);
    const uint64_t result = (static_cast<uint64_t>(value) + alignment - 1u) / alignment * alignment;
    return result > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(result);
}

bool isUndefined(const XffSymbol& symbol) { return symbol.sectionIndex == kShnUndef; }
bool isAbsolute(const XffSymbol& symbol) { return symbol.sectionIndex == kShnAbs; }
bool isExported(const XffSymbol& symbol) {
    return !symbol.name.empty() && symbol.binding != kStbLocal && !isUndefined(symbol);
}

void writeU32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value);
    data[offset + 1] = static_cast<uint8_t>(value >> 8u);
    data[offset + 2] = static_cast<uint8_t>(value >> 16u);
    data[offset + 3] = static_cast<uint8_t>(value >> 24u);
}

bool sectionOutputName(const XffModule& module, const XffSection& section,
                       std::string& name) {
    const std::string prefix = module.name;
    if (section.name == ".text") name = ".text." + prefix;
    else if (section.name == ".vutext") name = ".vutext." + prefix;
    else if (section.name == ".DVP.overlay") name = ".DVP.overlay." + prefix;
    else if (section.name == ".DVP.ovlytab") name = ".DVP.ovlytab." + prefix;
    else name = "." + prefix + section.name;
    return true;
}
}

bool parseXffModule(const std::string& name, const std::vector<uint8_t>& bytes,
                    XffModule& output, std::string& error) {
    output = {};
    output.name = name;
    output.data = bytes;
    if (bytes.size() < kHeaderSize ||
        bytes[0] != 'x' || bytes[1] != 'f' || bytes[2] != 'f' || bytes[3] != '2') {
        error = name + ": cabeçalho XFF2 ausente.";
        return false;
    }

    uint32_t fileSize = 0, symbolCount = 0, sectionCount = 0, relocationCount = 0;
    uint32_t firstGlobal = 0, symbolTableOffset = 0, stringTableOffset = 0;
    uint32_t sectionTableOffset = 0, relocationDescriptorOffset = 0;
    uint32_t sectionNameIndexOffset = 0, sectionNameStringsOffset = 0;
    uint32_t stringTableImagePointer = 0;

    if (!readU32(bytes, 0x0C, firstGlobal, name + " first global", error) ||
        !readU32(bytes, 0x14, fileSize, name + " file size", error) ||
        !readU32(bytes, 0x24, symbolCount, name + " symbol count", error) ||
        !readU32(bytes, 0x38, relocationCount, name + " relocation count", error) ||
        !readU32(bytes, 0x40, sectionCount, name + " section count", error) ||
        !readU32(bytes, 0x4C, output.entryOffset, name + " entry offset", error) ||
        !readU32(bytes, 0x54, symbolTableOffset, name + " symbol table offset", error) ||
        !readU32(bytes, 0x58, stringTableOffset, name + " string table offset", error) ||
        !readU32(bytes, 0x5C, sectionTableOffset, name + " section table offset", error) ||
        !readU32(bytes, 0x64, relocationDescriptorOffset, name + " relocation descriptor offset", error) ||
        !readU32(bytes, 0x68, sectionNameIndexOffset, name + " section name indexes", error) ||
        !readU32(bytes, 0x6C, sectionNameStringsOffset, name + " section name strings", error)) {
        return false;
    }
    if (fileSize != bytes.size()) {
        error = name + ": tamanho informado no cabeçalho não corresponde ao arquivo.";
        return false;
    }
    if (sectionCount == 0 || sectionCount > 512 || symbolCount > 1000000 ||
        relocationCount > 4096 || firstGlobal > symbolCount) {
        error = name + ": contagens XFF2 inválidas ou excessivas.";
        return false;
    }
    if (!checkedArray(bytes, sectionTableOffset, sectionCount, 32, name + " section headers", error) ||
        !checkedArray(bytes, symbolTableOffset, symbolCount, 16, name + " symbols", error) ||
        !checkedArray(bytes, sectionNameIndexOffset, sectionCount, 4, name + " section names", error) ||
        !checkedArray(bytes, relocationDescriptorOffset, relocationCount, 28, name + " relocation descriptors", error)) {
        return false;
    }

    output.sections.reserve(sectionCount);
    for (uint32_t i = 0; i < sectionCount; ++i) {
        const uint64_t offset = static_cast<uint64_t>(sectionTableOffset) + i * 32u;
        uint32_t runtime = 0, imagePointer = 0, size = 0, alignment = 0;
        uint32_t type = 0, flags = 0, fileOffset = 0, nameOffset = 0;
        if (!readU32(bytes, offset, runtime, name + " section runtime", error) ||
            !readU32(bytes, offset + 4, imagePointer, name + " section image pointer", error) ||
            !readU32(bytes, offset + 8, size, name + " section size", error) ||
            !readU32(bytes, offset + 12, alignment, name + " section alignment", error) ||
            !readU32(bytes, offset + 16, type, name + " section type", error) ||
            !readU32(bytes, offset + 20, flags, name + " section flags", error) ||
            !readU32(bytes, offset + 28, fileOffset, name + " section file offset", error) ||
            !readU32(bytes, sectionNameIndexOffset + i * 4u, nameOffset, name + " section name index", error)) {
            return false;
        }
        std::string sectionName;
        if (!readString(bytes, static_cast<uint64_t>(sectionNameStringsOffset) + nameOffset,
                        sectionName, name + " section name", error)) return false;

        XffSection section;
        section.index = i;
        section.name = std::move(sectionName);
        section.runtimeAddress = runtime;
        section.size = size;
        section.alignment = std::max(1u, alignment);
        section.type = type;
        section.flags = flags;
        section.fileOffset = fileOffset;
        if (imagePointer >= kMagicBias) {
            section.imageOffset = imagePointer - kMagicBias;
            section.hasImage = true;
        } else if (imagePointer != 0) {
            error = name + ":" + section.name + ": ponteiro de imagem XFF2 inválido.";
            return false;
        }
        if (section.hasImage && type != kShtNobits && size > 0) {
            if (!rangeOk(bytes.size(), section.imageOffset, size)) {
                error = name + ":" + section.name + ": dados da seção fora do arquivo.";
                return false;
            }
            if (fileOffset != section.imageOffset) {
                error = name + ":" + section.name + ": ponteiro da imagem diverge do offset de arquivo.";
                return false;
            }
        } else if (type != kShtNobits && size > 0 && (flags & kShfAlloc)) {
            error = name + ":" + section.name + ": seção alocável sem dados de imagem.";
            return false;
        }
        output.sections.push_back(std::move(section));
    }

    const auto textIt = std::find_if(output.sections.begin(), output.sections.end(),
        [](const XffSection& section) { return section.name == ".text"; });
    if (textIt == output.sections.end() || !(textIt->flags & kShfAlloc) ||
        textIt->type != kShtProgbits || output.entryOffset >= textIt->size ||
        (output.entryOffset & 3u) != 0) {
        error = name + ": .text ou entry offset inválido.";
        return false;
    }

    const auto strtabIt = std::find_if(output.sections.begin(), output.sections.end(),
        [](const XffSection& section) { return section.name == ".strtab"; });
    uint64_t stringLimit = bytes.size();
    if (strtabIt != output.sections.end() && strtabIt->hasImage) {
        stringLimit = static_cast<uint64_t>(strtabIt->imageOffset) + strtabIt->size;
        if (stringTableOffset != strtabIt->imageOffset) {
            error = name + ": ponteiro da tabela de strings diverge de .strtab.";
            return false;
        }
    }
    if (stringTableOffset > stringLimit) {
        error = name + ": tabela de strings fora dos limites.";
        return false;
    }

    output.symbols.reserve(symbolCount);
    for (uint32_t i = 0; i < symbolCount; ++i) {
        const uint64_t offset = static_cast<uint64_t>(symbolTableOffset) + i * 16u;
        uint32_t nameOffset = 0, value = 0, size = 0;
        uint8_t info = 0;
        uint16_t sectionIndex = 0;
        if (!readU32(bytes, offset, nameOffset, name + " symbol name", error) ||
            !readU32(bytes, offset + 4, value, name + " symbol value", error) ||
            !readU32(bytes, offset + 8, size, name + " symbol size", error) ||
            !readU16(bytes, offset + 14, sectionIndex, name + " symbol section", error)) {
            return false;
        }
        if (!rangeOk(bytes.size(), offset + 12, 1)) {
            error = name + ": info de símbolo fora dos limites.";
            return false;
        }
        info = bytes[offset + 12];
        XffSymbol symbol;
        symbol.index = i;
        symbol.value = value;
        symbol.size = size;
        symbol.binding = static_cast<uint8_t>(info >> 4u);
        symbol.type = static_cast<uint8_t>(info & 0x0Fu);
        symbol.sectionIndex = sectionIndex;
        if (nameOffset != 0) {
            if (!readString(bytes, static_cast<uint64_t>(stringTableOffset) + nameOffset,
                            symbol.name, name + " symbol name", error)) return false;
            if (static_cast<uint64_t>(stringTableOffset) + nameOffset >= stringLimit) {
                error = name + ": nome de símbolo ultrapassa .strtab.";
                return false;
            }
            const auto nul = std::find(bytes.begin() + stringTableOffset + nameOffset,
                                       bytes.begin() + static_cast<std::ptrdiff_t>(stringLimit), 0);
            if (nul == bytes.begin() + static_cast<std::ptrdiff_t>(stringLimit)) {
                error = name + ": nome de símbolo sem terminador em .strtab.";
                return false;
            }
        }
        if (sectionIndex < 0xFF00u && sectionIndex >= sectionCount) {
            error = name + ":" + (symbol.name.empty() ? std::to_string(i) : symbol.name) +
                    ": índice de seção de símbolo inválido.";
            return false;
        }
        output.symbols.push_back(std::move(symbol));
    }

    output.relocationTables.reserve(relocationCount);
    for (uint32_t tableIndex = 0; tableIndex < relocationCount; ++tableIndex) {
        const uint64_t offset = static_cast<uint64_t>(relocationDescriptorOffset) + tableIndex * 28u;
        uint32_t type = 0, count = 0, target = 0, entriesOffset = 0, originalsOffset = 0;
        if (!readU32(bytes, offset, type, name + " relocation type", error) ||
            !readU32(bytes, offset + 4, count, name + " relocation count", error) ||
            !readU32(bytes, offset + 8, target, name + " relocation target", error) ||
            !readU32(bytes, offset + 20, entriesOffset, name + " relocation entries", error) ||
            !readU32(bytes, offset + 24, originalsOffset, name + " relocation originals", error)) {
            return false;
        }
        if (type != kShtRel || target >= output.sections.size()) {
            error = name + ": tipo ou seção alvo de relocação não suportado.";
            return false;
        }
        if (!checkedArray(bytes, entriesOffset, count, 8, name + " relocation entries", error) ||
            !checkedArray(bytes, originalsOffset, count, 8, name + " relocation originals", error)) {
            return false;
        }
        const XffSection& targetSection = output.sections[target];
        if (!(targetSection.flags & kShfAlloc) || !targetSection.hasImage ||
            targetSection.type == kShtNobits) {
            error = name + ": relocação aponta para seção sem imagem alocável.";
            return false;
        }
        XffRelocationTable table;
        table.targetSection = target;
        table.entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t relocOffset = 0, info = 0, original = 0, auxiliary = 0;
            if (!readU32(bytes, entriesOffset + i * 8u, relocOffset, name + " relocation offset", error) ||
                !readU32(bytes, entriesOffset + i * 8u + 4u, info, name + " relocation info", error) ||
                !readU32(bytes, originalsOffset + i * 8u, original, name + " relocation original", error) ||
                !readU32(bytes, originalsOffset + i * 8u + 4u, auxiliary, name + " relocation auxiliary", error)) {
                return false;
            }
            const uint32_t symbolIndex = info >> 8u;
            const uint32_t relocationType = info & 0xFFu;
            if (symbolIndex >= output.symbols.size() ||
                (relocationType != 0 && relocationType != 2 && relocationType != 4 &&
                 relocationType != 5 && relocationType != 6) ||
                (relocOffset & 3u) != 0 || static_cast<uint64_t>(relocOffset) + 4u > targetSection.size) {
                error = name + ":" + targetSection.name + ": entrada de relocação inválida.";
                return false;
            }
            table.entries.push_back({relocOffset, symbolIndex, relocationType, original, auxiliary});
        }
        output.relocationTables.push_back(std::move(table));
    }

    error.clear();
    return true;
}

bool linkXffModules(const std::vector<XffModule>& modules,
                    const std::vector<uint32_t>& loadBases,
                    const std::vector<uint32_t>& bssBases,
                    uint32_t undefinedStub,
                    XffLinkResult& result,
                    std::string& error) {
    result = {};
    if (modules.empty() || modules.size() != loadBases.size() || modules.size() != bssBases.size()) {
        error = "Perfil de módulos XFF inválido.";
        return false;
    }

    for (size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        const XffModule& module = modules[moduleIndex];
        XffLinkedModule linked;
        linked.moduleIndex = moduleIndex;
        linked.base = loadBases[moduleIndex];
        linked.bssBase = bssBases[moduleIndex];
        linked.image = module.data;

        uint32_t bssCursor = linked.bssBase;
        for (const XffSection& section : module.sections) {
            if (!(section.flags & kShfAlloc) || section.size == 0) continue;
            if (section.type == kShtNobits) {
                bssCursor = alignValue(bssCursor, section.alignment);
                if (bssCursor == 0xFFFFFFFFu ||
                    static_cast<uint64_t>(bssCursor) + section.size > 0x100000000ull) {
                    error = module.name + ":" + section.name + ": BSS ultrapassa o espaço de 32 bits.";
                    return false;
                }
                linked.sectionAddresses[section.index] = bssCursor;
                bssCursor += section.size;
            } else if (section.hasImage) {
                const uint64_t address = static_cast<uint64_t>(linked.base) + section.imageOffset;
                if (address + section.size > 0x100000000ull) {
                    error = module.name + ":" + section.name + ": seção ultrapassa o espaço de 32 bits.";
                    return false;
                }
                linked.sectionAddresses[section.index] = static_cast<uint32_t>(address);
            } else {
                error = module.name + ":" + section.name + ": seção alocável sem endereço de imagem.";
                return false;
            }
        }

        const auto text = std::find_if(module.sections.begin(), module.sections.end(),
            [](const XffSection& section) { return section.name == ".text"; });
        if (text == module.sections.end() || linked.sectionAddresses.find(text->index) == linked.sectionAddresses.end()) {
            error = module.name + ": endereço da seção .text não foi carregado.";
            return false;
        }
        linked.entryAddress = linked.sectionAddresses[text->index] + module.entryOffset;

        auto resolveSymbol = [&](const XffSymbol& symbol, uint32_t& address, bool& unresolved) -> bool {
            unresolved = false;
            if (isAbsolute(symbol)) {
                address = symbol.value;
                return true;
            }
            if (isUndefined(symbol)) {
                const auto it = result.exports.find(symbol.name);
                if (it != result.exports.end()) address = it->second;
                else { address = undefinedStub; unresolved = true; }
                return true;
            }
            const auto sectionAddress = linked.sectionAddresses.find(symbol.sectionIndex);
            if (sectionAddress == linked.sectionAddresses.end() ||
                symbol.sectionIndex >= module.sections.size() ||
                symbol.value > module.sections[symbol.sectionIndex].size) {
                return false;
            }
            const uint64_t value = static_cast<uint64_t>(sectionAddress->second) + symbol.value;
            if (value > 0xFFFFFFFFu) return false;
            address = static_cast<uint32_t>(value);
            return true;
        };

        for (const XffSymbol& symbol : module.symbols) {
            if (isUndefined(symbol) || symbol.sectionIndex == kShnCommon) continue;
            uint32_t address = 0;
            bool unresolved = false;
            if (resolveSymbol(symbol, address, unresolved)) {
                linked.symbolAddresses[symbol.index] = address;
            }
        }

        for (const XffRelocationTable& table : module.relocationTables) {
            const XffSection& target = module.sections[table.targetSection];
            const auto targetAddress = linked.sectionAddresses.find(target.index);
            if (targetAddress == linked.sectionAddresses.end()) {
                error = module.name + ":" + target.name + ": endereço alvo de relocação ausente.";
                return false;
            }
            std::map<uint32_t, std::vector<std::pair<size_t, uint32_t>>> pendingHi;
            for (const XffRelocation& relocation : table.entries) {
                if (relocation.type == 0) continue;
                const XffSymbol& symbol = module.symbols[relocation.symbolIndex];
                uint32_t symbolAddress = 0;
                bool unresolved = false;
                if (!resolveSymbol(symbol, symbolAddress, unresolved)) {
                    error = module.name + ":" + symbol.name + ": símbolo aponta para seção não carregada.";
                    return false;
                }
                const size_t imagePosition = static_cast<size_t>(target.imageOffset) + relocation.offset;
                const uint32_t place = targetAddress->second + relocation.offset;
                const uint32_t original = relocation.originalWord;
                uint32_t patched = original;

                switch (relocation.type) {
                case 2: // R_MIPS_32
                    patched = original + symbolAddress;
                    writeU32(linked.image, imagePosition, patched);
                    break;
                case 4: { // R_MIPS_26
                    const uint32_t opcode = original >> 26u;
                    if (opcode != 2u && opcode != 3u) {
                        error = module.name + ":" + target.name + ": R_MIPS_26 não aponta para J/JAL.";
                        return false;
                    }
                    const uint32_t jumpTarget = ((original & 0x03FFFFFFu) << 2u) + symbolAddress;
                    if ((jumpTarget & 3u) != 0 ||
                        ((jumpTarget & 0xF0000000u) != ((place + 4u) & 0xF0000000u))) {
                        error = module.name + ":" + target.name + ": destino R_MIPS_26 inválido ou fora da região.";
                        return false;
                    }
                    patched = (original & 0xFC000000u) | ((jumpTarget >> 2u) & 0x03FFFFFFu);
                    writeU32(linked.image, imagePosition, patched);
                    break;
                }
                case 5: // R_MIPS_HI16
                    pendingHi[relocation.symbolIndex].push_back({imagePosition, original});
                    break;
                case 6: { // R_MIPS_LO16
                    int32_t low = static_cast<int32_t>(original & 0xFFFFu);
                    if ((low & 0x8000) != 0) low -= 0x10000;
                    auto pending = pendingHi.find(relocation.symbolIndex);
                    if (pending != pendingHi.end()) {
                        for (const auto& [hiPosition, hiOriginal] : pending->second) {
                            const int64_t addend = (static_cast<int64_t>(hiOriginal & 0xFFFFu) << 16u) + low;
                            const uint32_t value = static_cast<uint32_t>(static_cast<int64_t>(symbolAddress) + addend);
                            const uint32_t high = (value + 0x8000u) >> 16u;
                            writeU32(linked.image, hiPosition,
                                     (hiOriginal & 0xFFFF0000u) | (high & 0xFFFFu));
                        }
                        pendingHi.erase(pending);
                    }
                    patched = (original & 0xFFFF0000u) |
                              (static_cast<uint32_t>(symbolAddress + static_cast<uint32_t>(low)) & 0xFFFFu);
                    writeU32(linked.image, imagePosition, patched);
                    break;
                }
                default:
                    error = module.name + ":" + target.name + ": tipo de relocação não suportado.";
                    return false;
                }

                if (unresolved) {
                    ++linked.unresolvedImportCount;
                    uint32_t linkWord = 0;
                    readU32(linked.image, imagePosition, linkWord, "patched relocation", error);
                    result.unresolvedImports.push_back({
                        module.name, target.name, place, relocation.type,
                        symbol.name, original, linkWord
                    });
                }
            }
            if (!pendingHi.empty()) {
                error = module.name + ":" + target.name + ": relocação HI16 sem LO16 correspondente.";
                return false;
            }
        }

        // The loader's sequential global-symbol table lets a later module replace
        // an earlier export for modules loaded after it.
        for (const XffSymbol& symbol : module.symbols) {
            if (!isExported(symbol) || symbol.sectionIndex == kShnCommon) continue;
            uint32_t address = 0;
            bool unresolved = false;
            if (!resolveSymbol(symbol, address, unresolved) || unresolved) continue;
            auto previous = result.exports.find(symbol.name);
            if (previous != result.exports.end() && previous->second != address) {
                result.duplicateExports.push_back({symbol.name, previous->second, address, module.name});
            }
            result.exports[symbol.name] = address;
        }

        result.modules.push_back(std::move(linked));
    }

    error.clear();
    return true;
}
