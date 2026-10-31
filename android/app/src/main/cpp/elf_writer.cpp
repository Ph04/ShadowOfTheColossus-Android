#include "elf_writer.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;

namespace {
constexpr uint32_t kElfHeaderSize = 52;
constexpr uint32_t kProgramHeaderSize = 32;
constexpr uint32_t kSectionHeaderSize = 40;
constexpr uint32_t kEtExec = 2;
constexpr uint32_t kEmMips = 8;
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPfX = 1;
constexpr uint32_t kPfW = 2;
constexpr uint32_t kPfR = 4;
constexpr uint32_t kShtSymtab = 2;
constexpr uint32_t kShtStrtab = 3;
constexpr uint32_t kShtNobits = 8;
constexpr uint32_t kShfWrite = 1;
constexpr uint32_t kShfAlloc = 2;
constexpr uint32_t kShfExec = 4;
constexpr uint16_t kShnUndef = 0;
constexpr uint16_t kShnAbs = 0xFFF1;
constexpr uint8_t kStbLocal = 0;
constexpr uint8_t kSttFunc = 2;
constexpr uint8_t kSttSection = 3;

bool rangeOk(size_t total, uint64_t offset, uint64_t length) {
    return offset <= total && length <= static_cast<uint64_t>(total) - offset;
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (static_cast<uint16_t>(bytes[offset + 1]) << 8u));
}
uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24u);
}
void put16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
    out[offset] = static_cast<uint8_t>(value);
    out[offset + 1] = static_cast<uint8_t>(value >> 8u);
}
void put32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    out[offset] = static_cast<uint8_t>(value);
    out[offset + 1] = static_cast<uint8_t>(value >> 8u);
    out[offset + 2] = static_cast<uint8_t>(value >> 16u);
    out[offset + 3] = static_cast<uint8_t>(value >> 24u);
}
void append32(std::vector<uint8_t>& out, uint32_t value) {
    const size_t offset = out.size();
    out.resize(offset + 4);
    put32(out, offset, value);
}

std::string readCString(const std::vector<uint8_t>& bytes, size_t offset, size_t limit) {
    if (offset >= limit || limit > bytes.size()) return {};
    auto first = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    auto lastLimit = bytes.begin() + static_cast<std::ptrdiff_t>(limit);
    auto last = std::find(first, lastLimit, 0);
    if (last == lastLimit) return {};
    return std::string(first, last);
}

std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char ch : text) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch);
            else out << static_cast<char>(ch);
            break;
        }
    }
    return out.str();
}

std::string tomlEscape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        if (ch == '\n') { out += "\\n"; continue; }
        if (ch == '\r') { out += "\\r"; continue; }
        out.push_back(ch);
    }
    return out;
}

std::string csvField(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"') out += "\"\"";
        else out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

uint32_t powerOfTwoAlignment(uint32_t alignment) {
    if (alignment <= 1) return 1;
    if ((alignment & (alignment - 1u)) != 0) return 1;
    return std::min(alignment, 0x1000u);
}

uint32_t alignCongruent(uint32_t offset, uint32_t alignment, uint32_t address) {
    alignment = std::max(1u, alignment);
    const uint32_t remainder = offset % alignment;
    const uint32_t wanted = address % alignment;
    return offset + ((wanted + alignment - remainder) % alignment);
}

bool writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { error = "Não foi possível criar diretório: " + ec.message(); return false; }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { error = "Não foi possível criar " + path.string(); return false; }
    if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) { error = "Falha ao gravar " + path.string(); return false; }
    return true;
}

bool writeText(const fs::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { error = "Não foi possível criar diretório: " + ec.message(); return false; }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { error = "Não foi possível criar " + path.string(); return false; }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file) { error = "Falha ao gravar " + path.string(); return false; }
    return true;
}

std::string moduleSectionName(const XffModule& module, const XffSection& section) {
    if (section.name == ".text") return ".text." + module.name;
    if (section.name == ".vutext") return ".vutext." + module.name;
    if (section.name == ".DVP.overlay") return ".DVP.overlay." + module.name;
    if (section.name == ".DVP.ovlytab") return ".DVP.ovlytab." + module.name;
    return "." + module.name + section.name;
}

std::string hex32(uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

bool isCpuCodeName(const std::string& name) {
    if (name == ".boot.text") return true;
    if (name.rfind(".text.", 0) == 0) return true;
    // VU/DVP sections are executable to the linker, but are not EE R5900 code.
    return false;
}

struct OutputProgramHeader {
    std::array<uint32_t, 8> values{};
};
struct OutputSectionHeader {
    std::array<uint32_t, 10> values{};
};

bool writeElfFile(const SotcElfResult& image, const fs::path& path, std::string& error) {
    if (image.sections.empty()) { error = "Não há seções para criar o ELF."; return false; }
    std::set<std::string> names;
    for (const auto& section : image.sections) {
        if (!(section.flags & kShfAlloc)) {
            error = "A seção " + section.name + " não está marcada como alocável.";
            return false;
        }
        if (!names.insert(section.name).second) {
            error = "Nome de seção ELF duplicado: " + section.name;
            return false;
        }
        if (static_cast<uint64_t>(section.address) + section.size > 0x100000000ull) {
            error = "Endereço de seção ultrapassa 32 bits: " + section.name;
            return false;
        }
        if (section.type == kShtNobits) {
            if (section.hasData) { error = section.name + ": BSS não pode conter payload de arquivo."; return false; }
        } else if (!section.hasData || section.data.size() != section.size) {
            error = section.name + ": tamanho do payload não corresponde ao cabeçalho.";
            return false;
        }
    }

    const uint32_t sectionCount = static_cast<uint32_t>(image.sections.size());
    const uint32_t symtabIndex = sectionCount + 1u;
    const uint32_t strtabIndex = symtabIndex + 1u;
    const uint32_t shstrtabIndex = strtabIndex + 1u;
    const uint32_t shnum = shstrtabIndex + 1u;
    std::vector<OutputProgramHeader> programHeaders;
    std::vector<OutputSectionHeader> sectionHeaders;
    sectionHeaders.push_back({});

    std::vector<uint8_t> body(kElfHeaderSize + sectionCount * kProgramHeaderSize, 0);
    for (const auto& section : image.sections) {
        const uint32_t alignment = powerOfTwoAlignment(section.alignment);
        const uint32_t fileOffset = alignCongruent(static_cast<uint32_t>(body.size()), alignment, section.address);
        if (fileOffset < body.size()) { error = "Overflow ao alinhar ELF."; return false; }
        body.resize(fileOffset, 0);
        if (section.type != kShtNobits && section.hasData) {
            body.insert(body.end(), section.data.begin(), section.data.end());
        }

        uint32_t flags = kPfR;
        if (section.flags & kShfWrite) flags |= kPfW;
        if (section.flags & kShfExec) flags |= kPfX;
        programHeaders.push_back({{{kPtLoad, fileOffset, section.address, section.address,
            section.type == kShtNobits ? 0u : section.size, section.size, flags, alignment}}});
        sectionHeaders.push_back({{{0, section.type, section.flags, section.address,
            fileOffset, section.size, 0, 0, std::max(1u, section.alignment), 0}}});
    }

    std::vector<SotcImageSymbol> symbols = image.symbols;
    std::stable_sort(symbols.begin(), symbols.end(), [](const auto& a, const auto& b) {
        return std::make_tuple(a.binding != kStbLocal, a.value, a.name) <
               std::make_tuple(b.binding != kStbLocal, b.value, b.name);
    });

    std::vector<uint8_t> strtab{0};
    std::vector<uint8_t> symtab(16, 0);
    uint32_t firstGlobal = static_cast<uint32_t>(symbols.size()) + 1u;
    bool foundGlobal = false;
    for (size_t i = 0; i < symbols.size(); ++i) {
        const SotcImageSymbol& symbol = symbols[i];
        if (symbol.sectionIndex != kShnUndef && symbol.sectionIndex != kShnAbs &&
            (symbol.sectionIndex == 0 || symbol.sectionIndex > sectionCount)) {
            error = symbol.name + ": índice de seção inválido na tabela ELF.";
            return false;
        }
        const uint32_t nameOffset = static_cast<uint32_t>(strtab.size());
        strtab.insert(strtab.end(), symbol.name.begin(), symbol.name.end());
        strtab.push_back(0);
        const size_t offset = symtab.size();
        symtab.resize(offset + 16, 0);
        put32(symtab, offset, nameOffset);
        put32(symtab, offset + 4, symbol.value);
        put32(symtab, offset + 8, symbol.size);
        symtab[offset + 12] = static_cast<uint8_t>((symbol.binding << 4u) | (symbol.type & 0x0Fu));
        put16(symtab, offset + 14, symbol.sectionIndex);
        if (!foundGlobal && symbol.binding != kStbLocal) {
            firstGlobal = static_cast<uint32_t>(i) + 1u;
            foundGlobal = true;
        }
    }

    while ((body.size() & 3u) != 0) body.push_back(0);
    const uint32_t symtabOffset = static_cast<uint32_t>(body.size());
    body.insert(body.end(), symtab.begin(), symtab.end());
    const uint32_t strtabOffset = static_cast<uint32_t>(body.size());
    body.insert(body.end(), strtab.begin(), strtab.end());

    std::vector<uint8_t> shstrtab{0};
    std::vector<uint32_t> sectionNameOffsets;
    for (const auto& section : image.sections) {
        sectionNameOffsets.push_back(static_cast<uint32_t>(shstrtab.size()));
        shstrtab.insert(shstrtab.end(), section.name.begin(), section.name.end());
        shstrtab.push_back(0);
    }
    const uint32_t symtabNameOffset = static_cast<uint32_t>(shstrtab.size());
    const std::string symtabName = ".symtab";
    shstrtab.insert(shstrtab.end(), symtabName.begin(), symtabName.end());
    shstrtab.push_back(0);
    const uint32_t strtabNameOffset = static_cast<uint32_t>(shstrtab.size());
    const std::string strtabName = ".strtab";
    shstrtab.insert(shstrtab.end(), strtabName.begin(), strtabName.end());
    shstrtab.push_back(0);
    const uint32_t shstrtabNameOffset = static_cast<uint32_t>(shstrtab.size());
    const std::string shstrtabName = ".shstrtab";
    shstrtab.insert(shstrtab.end(), shstrtabName.begin(), shstrtabName.end());
    shstrtab.push_back(0);
    const uint32_t shstrtabOffset = static_cast<uint32_t>(body.size());
    body.insert(body.end(), shstrtab.begin(), shstrtab.end());

    for (size_t i = 0; i < image.sections.size(); ++i) {
        sectionHeaders[i + 1].values[0] = sectionNameOffsets[i];
    }
    sectionHeaders.push_back({{{symtabNameOffset, kShtSymtab, 0, 0, symtabOffset,
        static_cast<uint32_t>(symtab.size()), strtabIndex, firstGlobal, 4, 16}}});
    sectionHeaders.push_back({{{strtabNameOffset, kShtStrtab, 0, 0, strtabOffset,
        static_cast<uint32_t>(strtab.size()), 0, 0, 1, 0}}});
    sectionHeaders.push_back({{{shstrtabNameOffset, kShtStrtab, 0, 0, shstrtabOffset,
        static_cast<uint32_t>(shstrtab.size()), 0, 0, 1, 0}}});

    const uint32_t sectionHeaderOffset = static_cast<uint32_t>(body.size());
    for (const auto& header : sectionHeaders) {
        const size_t offset = body.size();
        body.resize(offset + kSectionHeaderSize, 0);
        for (size_t i = 0; i < 10; ++i) put32(body, offset + i * 4, header.values[i]);
    }

    for (size_t i = 0; i < programHeaders.size(); ++i) {
        for (size_t j = 0; j < 8; ++j) {
            put32(body, kElfHeaderSize + i * kProgramHeaderSize + j * 4, programHeaders[i].values[j]);
        }
    }

    body[0] = 0x7F; body[1] = 'E'; body[2] = 'L'; body[3] = 'F';
    body[4] = 1; body[5] = 1; body[6] = 1;
    put16(body, 16, kEtExec);
    put16(body, 18, kEmMips);
    put32(body, 20, 1);
    put32(body, 24, image.entry);
    put32(body, 28, kElfHeaderSize);
    put32(body, 32, sectionHeaderOffset);
    put32(body, 36, image.flags);
    put16(body, 40, kElfHeaderSize);
    put16(body, 42, kProgramHeaderSize);
    put16(body, 44, static_cast<uint16_t>(programHeaders.size()));
    put16(body, 46, kSectionHeaderSize);
    put16(body, 48, static_cast<uint16_t>(shnum));
    put16(body, 50, static_cast<uint16_t>(shstrtabIndex));

    return writeBytes(path, body, error);
}
}

bool buildSotcElf(const std::vector<uint8_t>& bootElf,
                  const std::vector<XffModule>& modules,
                  const XffLinkResult& linked,
                  SotcElfResult& image,
                  std::string& error) {
    image = {};
    if (bootElf.size() < kElfHeaderSize || bootElf[0] != 0x7F || bootElf[1] != 'E' ||
        bootElf[2] != 'L' || bootElf[3] != 'F' || bootElf[4] != 1 ||
        bootElf[5] != 1 || bootElf[6] != 1) {
        error = "O executável de boot não é ELF32 little-endian.";
        return false;
    }
    const uint16_t elfType = read16(bootElf, 16);
    const uint16_t machine = read16(bootElf, 18);
    const uint32_t version = read32(bootElf, 20);
    image.entry = read32(bootElf, 24);
    image.flags = read32(bootElf, 36);
    const uint32_t sectionOffset = read32(bootElf, 32);
    const uint16_t sectionEntrySize = read16(bootElf, 46);
    const uint16_t sectionCount = read16(bootElf, 48);
    const uint16_t stringIndex = read16(bootElf, 50);
    if (elfType != kEtExec || machine != kEmMips || version != 1 ||
        sectionEntrySize < kSectionHeaderSize || sectionCount == 0 || stringIndex >= sectionCount ||
        !rangeOk(bootElf.size(), sectionOffset, static_cast<uint64_t>(sectionEntrySize) * sectionCount)) {
        error = "Cabeçalhos ELF do executável de boot são inválidos.";
        return false;
    }

    struct BootHeader { uint32_t name, type, flags, address, offset, size, link, info, alignment, entrySize; };
    std::vector<BootHeader> headers;
    headers.reserve(sectionCount);
    for (uint32_t i = 0; i < sectionCount; ++i) {
        const size_t offset = sectionOffset + i * sectionEntrySize;
        BootHeader h{
            read32(bootElf, offset), read32(bootElf, offset + 4), read32(bootElf, offset + 8),
            read32(bootElf, offset + 12), read32(bootElf, offset + 16), read32(bootElf, offset + 20),
            read32(bootElf, offset + 24), read32(bootElf, offset + 28), read32(bootElf, offset + 32),
            read32(bootElf, offset + 36)
        };
        headers.push_back(h);
    }
    const BootHeader& shstrHeader = headers[stringIndex];
    if (shstrHeader.type != kShtStrtab ||
        !rangeOk(bootElf.size(), shstrHeader.offset, shstrHeader.size)) {
        error = "Tabela de nomes de seções do boot ELF é inválida.";
        return false;
    }

    std::map<uint32_t, uint16_t> bootSectionIndexByInput;
    std::map<std::string, uint16_t> outputSectionIndexByName;
    for (size_t i = 1; i < headers.size(); ++i) {
        const BootHeader& h = headers[i];
        if (!(h.flags & kShfAlloc) || h.size == 0) continue;
        const std::string originalName = readCString(bootElf, shstrHeader.offset + h.name,
                                                      shstrHeader.offset + shstrHeader.size);
        if (originalName.empty()) continue;
        SotcImageSection section;
        section.name = ".boot." + (originalName.front() == '.' ? originalName.substr(1) : originalName);
        section.address = h.address;
        section.size = h.size;
        section.type = h.type;
        section.flags = h.flags;
        section.alignment = std::max(1u, h.alignment);
        section.isCode = (h.flags & kShfExec) && originalName == ".text";
        section.hasData = h.type != kShtNobits;
        if (section.hasData) {
            if (!rangeOk(bootElf.size(), h.offset, h.size)) {
                error = "Seção " + section.name + " ultrapassa o executável de boot.";
                return false;
            }
            section.data.assign(bootElf.begin() + h.offset, bootElf.begin() + h.offset + h.size);
        }
        outputSectionIndexByName[section.name] = static_cast<uint16_t>(image.sections.size() + 1);
        bootSectionIndexByInput[static_cast<uint32_t>(i)] = static_cast<uint16_t>(image.sections.size() + 1);
        image.sections.push_back(std::move(section));
    }
    if (image.sections.empty()) {
        error = "O ELF de boot não contém seções alocáveis.";
        return false;
    }
    const bool entryInText = std::any_of(image.sections.begin(), image.sections.end(),
        [&](const SotcImageSection& s) {
            return s.isCode && image.entry >= s.address && image.entry < s.address + s.size;
        });
    if (!entryInText) {
        error = "Entry point do ELF de boot não está dentro de uma seção .text executável.";
        return false;
    }

    for (size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        if (moduleIndex >= linked.modules.size()) {
            error = "Resultado de ligação XFF incompleto.";
            return false;
        }
        const XffModule& module = modules[moduleIndex];
        const XffLinkedModule& linkedModule = linked.modules[moduleIndex];
        for (const XffSection& source : module.sections) {
            if (!(source.flags & kShfAlloc) || source.size == 0) continue;
            const auto address = linkedModule.sectionAddresses.find(source.index);
            if (address == linkedModule.sectionAddresses.end()) {
                error = module.name + ":" + source.name + ": seção alocável sem endereço.";
                return false;
            }
            SotcImageSection section;
            section.name = moduleSectionName(module, source);
            section.address = address->second;
            section.size = source.size;
            section.type = source.type;
            section.flags = source.flags;
            section.alignment = std::max(1u, source.alignment);
            section.hasData = source.type != kShtNobits;
            section.isCode = (source.flags & kShfExec) && source.name == ".text";
            if (section.hasData) {
                if (!source.hasImage || !rangeOk(linkedModule.image.size(), source.imageOffset, source.size)) {
                    error = module.name + ":" + source.name + ": dados da seção ligada estão fora da imagem.";
                    return false;
                }
                section.data.assign(linkedModule.image.begin() + source.imageOffset,
                                    linkedModule.image.begin() + source.imageOffset + source.size);
            }
            if (outputSectionIndexByName.contains(section.name)) {
                error = "Nome de seção repetido: " + section.name;
                return false;
            }
            outputSectionIndexByName[section.name] = static_cast<uint16_t>(image.sections.size() + 1);
            image.sections.push_back(std::move(section));
        }
    }

    // Retain boot symbols if the original ELF has a usable SYMTAB.
    for (size_t i = 1; i < headers.size(); ++i) {
        const BootHeader& table = headers[i];
        if (table.type != kShtSymtab || table.entrySize < 16 || table.link >= headers.size() ||
            !rangeOk(bootElf.size(), table.offset, table.size)) continue;
        const BootHeader& strings = headers[table.link];
        if (strings.type != kShtStrtab || !rangeOk(bootElf.size(), strings.offset, strings.size)) continue;
        const size_t count = table.size / table.entrySize;
        for (size_t j = 1; j < count; ++j) {
            const size_t offset = table.offset + j * table.entrySize;
            const uint32_t nameOffset = read32(bootElf, offset);
            const uint32_t value = read32(bootElf, offset + 4);
            const uint32_t size = read32(bootElf, offset + 8);
            const uint8_t info = bootElf[offset + 12];
            const uint16_t shndx = read16(bootElf, offset + 14);
            const std::string name = nameOffset ? readCString(bootElf, strings.offset + nameOffset,
                strings.offset + strings.size) : std::string();
            if (name.empty()) continue;
            SotcImageSymbol symbol;
            symbol.name = name;
            symbol.value = value;
            symbol.size = size;
            symbol.binding = info >> 4u;
            symbol.type = info & 0x0Fu;
            if (shndx == kShnAbs) symbol.sectionIndex = kShnAbs;
            else if (shndx == kShnUndef) symbol.sectionIndex = kShnUndef;
            else {
                const auto mapped = bootSectionIndexByInput.find(shndx);
                if (mapped == bootSectionIndexByInput.end()) continue;
                symbol.sectionIndex = mapped->second;
            }
            image.symbols.push_back(std::move(symbol));
        }
    }

    // Export defined and imported XFF symbols into the merged ELF symbol table.
    for (size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        const XffModule& module = modules[moduleIndex];
        const XffLinkedModule& linkedModule = linked.modules[moduleIndex];
        for (const XffSymbol& source : module.symbols) {
            if (source.name.empty() || source.type == kSttSection) continue;
            SotcImageSymbol symbol;
            symbol.name = source.name;
            symbol.size = source.size;
            symbol.binding = source.binding;
            symbol.type = source.type;
            if (source.sectionIndex == kShnUndef) {
                symbol.sectionIndex = kShnUndef;
                image.symbols.push_back(std::move(symbol));
                continue;
            }
            if (source.sectionIndex == kShnAbs) {
                symbol.value = source.value;
                symbol.sectionIndex = kShnAbs;
                image.symbols.push_back(std::move(symbol));
                continue;
            }
            const auto address = linkedModule.symbolAddresses.find(source.index);
            if (address == linkedModule.symbolAddresses.end()) continue;
            symbol.value = address->second;
            const std::string outputName = moduleSectionName(module, module.sections[source.sectionIndex]);
            const auto index = outputSectionIndexByName.find(outputName);
            if (index == outputSectionIndexByName.end()) continue;
            symbol.sectionIndex = index->second;
            image.symbols.push_back(std::move(symbol));
        }
    }

    error.clear();
    return true;
}

bool writeSotcElf(const SotcElfResult& image, const std::string& path,
                  std::string& error) {
    return writeElfFile(image, fs::path(path), error);
}

bool writeSotcRecompilerInputs(const SotcElfResult& image,
                               const std::vector<XffModule>& modules,
                               const XffLinkResult& linked,
                               const std::string& outputDirectory,
                               std::string& error) {
    const fs::path root(outputDirectory);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) { error = "Não foi possível criar a pasta de saída: " + ec.message(); return false; }

    const fs::path elfPath = root / "sotc_linked.elf";
    if (!writeSotcElf(image, elfPath.string(), error)) return false;

    // Build a named function inventory. The PS2Recomp config intentionally leaves
    // ghidra_output empty so its own heuristic discovery is not suppressed.
    std::map<uint32_t, std::pair<SotcImageSymbol, const SotcImageSection*>> functions;
    for (const SotcImageSymbol& symbol : image.symbols) {
        if (symbol.type != kSttFunc || symbol.name.empty() || symbol.value == 0) continue;
        const SotcImageSection* section = nullptr;
        if (symbol.sectionIndex == kShnAbs) {
            for (const auto& candidate : image.sections) {
                if (candidate.isCode && symbol.value >= candidate.address &&
                    symbol.value < candidate.address + candidate.size) { section = &candidate; break; }
            }
        } else if (symbol.sectionIndex > 0 && symbol.sectionIndex <= image.sections.size()) {
            const auto& candidate = image.sections[symbol.sectionIndex - 1];
            if (candidate.isCode && symbol.value >= candidate.address &&
                symbol.value < candidate.address + candidate.size) section = &candidate;
        }
        if (!section) continue;
        auto found = functions.find(symbol.value);
        if (found == functions.end() || (found->second.first.size == 0 && symbol.size > 0)) {
            functions[symbol.value] = {symbol, section};
        }
    }

    std::ostringstream functionCsv;
    functionCsv << "name,start,end,size\n";
    for (auto it = functions.begin(); it != functions.end(); ++it) {
        const uint32_t start = it->first;
        const auto& symbol = it->second.first;
        const SotcImageSection* section = it->second.second;
        auto next = std::next(it);
        while (next != functions.end() && next->second.second != section) ++next;
        uint32_t end = section->address + section->size;
        if (next != functions.end()) end = next->first;
        if (symbol.size > 0) end = std::min(end, start + symbol.size);
        if (end <= start) continue;
        functionCsv << csvField(symbol.name) << "," << hex32(start) << ","
                    << hex32(end) << "," << (end - start) << "\n";
    }
    if (!writeText(root / "sotc_functions.csv", functionCsv.str(), error)) return false;

    std::ostringstream runtimeCsv;
    runtimeCsv << "address,kind,module,symbol\n";
    size_t runtimeCodeSites = 0;
    size_t skippedWordSites = 0;
    std::ostringstream runtimeJson;
    runtimeJson << "{\n  \"format\": \"sotc-runtime-import-sites-v1\",\n"
                << "  \"profile\": \"SCES-53326 v1.00\",\n  \"entries\": [\n";
    bool firstImport = true;
    for (size_t moduleIndex = 0; moduleIndex < modules.size(); ++moduleIndex) {
        const XffModule& module = modules[moduleIndex];
        const XffLinkedModule& linkedModule = linked.modules[moduleIndex];
        for (const XffRelocationTable& table : module.relocationTables) {
            const XffSection& section = module.sections[table.targetSection];
            const auto addressIt = linkedModule.sectionAddresses.find(section.index);
            if (addressIt == linkedModule.sectionAddresses.end()) continue;
            for (const XffRelocation& relocation : table.entries) {
                const XffSymbol& symbol = module.symbols[relocation.symbolIndex];
                if (symbol.sectionIndex != kShnUndef) continue;
                const uint32_t address = addressIt->second + relocation.offset;
                uint32_t word = relocation.originalWord;
                if (section.hasImage && rangeOk(linkedModule.image.size(),
                    static_cast<uint64_t>(section.imageOffset) + relocation.offset, 4)) {
                    const size_t pos = section.imageOffset + relocation.offset;
                    word = read32(linkedModule.image, pos);
                }
                std::string kind = "mips_" + std::to_string(relocation.type);
                if (relocation.type == 2) kind = "w32";
                else if (relocation.type == 4) kind = "j26";
                else if (relocation.type == 5) kind = "hi16";
                else if (relocation.type == 6) kind = "lo16";

                if (!firstImport) runtimeJson << ",\n";
                firstImport = false;
                runtimeJson << "    {\"module\":\"" << jsonEscape(module.name)
                    << "\",\"section\":\"" << jsonEscape(section.name)
                    << "\",\"address\":\"" << hex32(address)
                    << "\",\"kind\":\"" << kind
                    << "\",\"symbol\":\"" << jsonEscape(symbol.name)
                    << "\",\"original_word\":\"" << hex32(relocation.originalWord)
                    << "\",\"link_time_word\":\"" << hex32(word) << "\"}";

                if (section.name == ".text") {
                    if (relocation.type == 4) {
                        runtimeCsv << hex32(address) << ",j26," << csvField(module.name) << "," << csvField(symbol.name) << "\n";
                        ++runtimeCodeSites;
                    } else if (relocation.type == 5) {
                        runtimeCsv << hex32(address) << ",hi16," << csvField(module.name) << "," << csvField(symbol.name) << "\n";
                        ++runtimeCodeSites;
                    } else if (relocation.type == 6) {
                        runtimeCsv << hex32(address) << ",lo16," << csvField(module.name) << "," << csvField(symbol.name) << "\n";
                        ++runtimeCodeSites;
                    } else if (relocation.type == 2) {
                        ++skippedWordSites;
                    }
                }
            }
        }
    }
    runtimeJson << "\n  ]\n}\n";
    if (!writeText(root / "runtime_import_sites.json", runtimeJson.str(), error) ||
        !writeText(root / "runtime_relocations.csv", runtimeCsv.str(), error)) return false;

    std::ostringstream unresolvedJson;
    unresolvedJson << "{\n  \"format\": \"sotc-static-unresolved-imports-v1\",\n"
                   << "  \"undefined_import_stub\": \"0x001B28F8\",\n  \"entries\": [\n";
    for (size_t i = 0; i < linked.unresolvedImports.size(); ++i) {
        const auto& entry = linked.unresolvedImports[i];
        if (i) unresolvedJson << ",\n";
        unresolvedJson << "    {\"module\":\"" << jsonEscape(entry.module)
            << "\",\"section\":\"" << jsonEscape(entry.section)
            << "\",\"address\":\"" << hex32(entry.address)
            << "\",\"relocation\":" << entry.relocationType
            << ",\"symbol\":\"" << jsonEscape(entry.symbol)
            << "\",\"original_word\":\"" << hex32(entry.originalWord)
            << "\",\"link_time_word\":\"" << hex32(entry.linkTimeWord) << "\"}";
    }
    unresolvedJson << "\n  ]\n}\n";
    if (!writeText(root / "runtime_relocations.json", unresolvedJson.str(), error)) return false;

    std::ostringstream config;
    config << "[general]\n"
           << "input = \"" << tomlEscape(elfPath.string()) << "\"\n"
           << "ghidra_output = \"\"\n"
           << "runtime_relocations = \"" << tomlEscape((root / "runtime_relocations.csv").string()) << "\"\n"
           << "output = \"" << tomlEscape((root / "generated").string()) << "\"\n"
           << "single_file_output = false\n"
           << "output_worker_threads = 1\n"
           << "low_memory_mode = true\n"
           << "patch_syscalls = false\n"
           << "patch_cop0 = false\n"
           << "patch_cache = false\n"
           << "auto_stub_by_name = false\n"
           << "direct_tail_calls = false\n"
           << "stubs = []\n"
           << "skip = []\n";
    if (!writeText(root / "sotc_recomp.toml", config.str(), error)) return false;

    std::ostringstream manifest;
    manifest << "{\n"
        << "  \"format\": \"sotc-on-device-pipeline-v1\",\n"
        << "  \"profile\": \"SCES-53326 v1.00\",\n"
        << "  \"boot_entry\": \"" << hex32(image.entry) << "\",\n"
        << "  \"merged_elf\": \"sotc_linked.elf\",\n"
        << "  \"merged_elf_bytes\": " << fs::file_size(elfPath, ec) << ",\n"
        << "  \"sections\": " << image.sections.size() << ",\n"
        << "  \"symbols\": " << image.symbols.size() << ",\n"
        << "  \"named_functions\": " << functions.size() << ",\n"
        << "  \"runtime_import_sites\": " << linked.unresolvedImports.size() << ",\n"
        << "  \"ps2recomp_runtime_sites\": " << runtimeCodeSites << ",\n"
        << "  \"R_MIPS_32_sites_not_consumed_by_upstream_runtime_map\": " << skippedWordSites << ",\n"
        << "  \"unresolved_imports\": " << linked.unresolvedImports.size() << ",\n"
        << "  \"resolved_exports\": " << linked.exports.size() << ",\n"
        << "  \"note\": \"Arquivos gerados localmente no dispositivo; ainda requer comparação com dump de RAM e integração do runtime GS Android.\"\n"
        << "}\n";
    if (!writeText(root / "link_manifest.json", manifest.str(), error)) return false;

    error.clear();
    return true;
}
