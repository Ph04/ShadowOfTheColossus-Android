#include "iso9660.h"
#include "sha256.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace {
constexpr size_t kMaximumDirectoryBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumEntries = 50000;
constexpr unsigned kMaximumDepth = 16;

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8u));
}
uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}
uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) | p[1]);
}
uint32_t readBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24u) |
           (static_cast<uint32_t>(p[1]) << 16u) |
           (static_cast<uint32_t>(p[2]) << 8u) |
           static_cast<uint32_t>(p[3]);
}

bool readBoth32(const uint8_t* p, uint32_t& value) {
    value = readLe32(p);
    return value == readBe32(p + 4);
}
bool readBoth16(const uint8_t* p, uint16_t& value) {
    value = readLe16(p);
    return value == readBe16(p + 2);
}

std::string normalizeComponent(std::string value) {
    const size_t version = value.find(';');
    if (version != std::string::npos) value.resize(version);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\\0')) value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string normalizePath(std::string_view input) {
    std::string result;
    std::string component;
    auto flush = [&]() {
        const std::string normalized = normalizeComponent(component);
        if (!normalized.empty()) {
            if (!result.empty()) result.push_back('/');
            result += normalized;
        }
        component.clear();
    };
    for (char c : input) {
        if (c == '/' || c == '\\\\') flush();
        else component.push_back(c);
    }
    flush();
    return result;
}
}

Iso9660Reader::Iso9660Reader(const IsoSource& source) : source_(source) {}

bool Iso9660Reader::rangeValid(uint32_t lba, uint32_t size) const {
    const uint64_t offset = static_cast<uint64_t>(lba) * kSectorSize;
    return offset <= static_cast<uint64_t>(source_.size()) &&
           static_cast<uint64_t>(size) <= static_cast<uint64_t>(source_.size()) - offset;
}

bool Iso9660Reader::open(std::string& error) {
    opened_ = false;
    entries_.clear();
    volumeId_.clear();
    volumeSectors_ = 0;
    if (!source_.isOpen() || source_.size() < 17ll * kSectorSize) {
        error = "A imagem não contém setores suficientes para um volume ISO9660.";
        return false;
    }

    std::array<uint8_t, kSectorSize> sector{};
    bool foundPrimary = false;
    for (uint32_t lba = 16; lba < 80; ++lba) {
        size_t count = 0;
        if (!source_.readAt(static_cast<uint64_t>(lba) * kSectorSize,
                            sector.data(), sector.size(), count)) {
            error = "Não foi possível ler os descritores de volume ISO9660.";
            return false;
        }
        if (std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[6] != 1) {
            error = "Descritor ISO9660 inválido (assinatura CD001 ausente).";
            return false;
        }
        if (sector[0] == 255) break;
        if (sector[0] != 1) continue;

        uint16_t blockSize = 0;
        if (!readBoth16(sector.data() + 128, blockSize) || blockSize != kSectorSize ||
            !readBoth32(sector.data() + 80, volumeSectors_)) {
            error = "Descritor primário ISO9660 inconsistente.";
            return false;
        }
        volumeId_.assign(reinterpret_cast<const char*>(sector.data() + 40), 32);
        while (!volumeId_.empty() && volumeId_.back() == ' ') volumeId_.pop_back();

        const uint8_t* root = sector.data() + 156;
        if (root[0] < 34 || root[0] > 34 || (root[25] & 0x02u) == 0) {
            error = "Registro do diretório raiz ISO9660 inválido.";
            return false;
        }
        uint32_t rootLba = 0, rootSize = 0;
        if (!readBoth32(root + 2, rootLba) || !readBoth32(root + 10, rootSize) ||
            !rangeValid(rootLba, rootSize)) {
            error = "Extensão do diretório raiz ultrapassa a imagem.";
            return false;
        }
        foundPrimary = true;
        if (!walkDirectory(rootLba, rootSize, "", 0, error)) return false;
        break;
    }

    if (!foundPrimary) {
        error = "A imagem não contém um descritor primário ISO9660.";
        return false;
    }
    opened_ = true;
    error.clear();
    return true;
}

bool Iso9660Reader::walkDirectory(uint32_t lba, uint32_t size, const std::string& prefix,
                                  unsigned depth, std::string& error) {
    if (depth > kMaximumDepth || size > kMaximumDirectoryBytes || !rangeValid(lba, size)) {
        error = "Diretório ISO9660 fora dos limites ou profundo demais.";
        return false;
    }
    const uint64_t key = (static_cast<uint64_t>(lba) << 32u) | size;
    static thread_local std::unordered_set<uint64_t> activeDirectories;
    if (depth == 0) activeDirectories.clear();
    if (!activeDirectories.insert(key).second) return true;

    std::vector<uint8_t> data(size);
    size_t count = 0;
    if (!source_.readAt(static_cast<uint64_t>(lba) * kSectorSize, data.data(), data.size(), count)) {
        activeDirectories.erase(key);
        error = "Falha ao ler um diretório ISO9660.";
        return false;
    }

    size_t offset = 0;
    while (offset < data.size()) {
        const uint8_t recordSize = data[offset];
        if (recordSize == 0) {
            offset = ((offset / kSectorSize) + 1u) * kSectorSize;
            continue;
        }
        if (recordSize < 34 || offset + recordSize > data.size()) {
            activeDirectories.erase(key);
            error = "Registro de diretório ISO9660 malformado.";
            return false;
        }

        const uint8_t* record = data.data() + offset;
        uint32_t fileLba = 0, fileSize = 0;
        const uint8_t nameLength = record[32];
        if (33u + nameLength > recordSize ||
            !readBoth32(record + 2, fileLba) || !readBoth32(record + 10, fileSize)) {
            activeDirectories.erase(key);
            error = "Campos de registro ISO9660 inconsistentes.";
            return false;
        }
        const uint8_t flags = record[25];
        const bool isDirectory = (flags & 0x02u) != 0;
        const bool multiExtent = (flags & 0x80u) != 0;
        const uint8_t* nameBytes = record + 33;
        offset += recordSize;

        if (nameLength == 1 && (nameBytes[0] == 0 || nameBytes[0] == 1)) continue;
        if (multiExtent) {
            activeDirectories.erase(key);
            error = "A imagem usa arquivo ISO9660 multi-extent, ainda não suportado.";
            return false;
        }

        std::string name(reinterpret_cast<const char*>(nameBytes), nameLength);
        name = normalizeComponent(name);
        if (name.empty()) continue;
        const std::string fullPath = prefix.empty() ? name : prefix + "/" + name;
        if (!rangeValid(fileLba, fileSize)) {
            activeDirectories.erase(key);
            error = "Uma entrada ISO9660 aponta para fora do arquivo.";
            return false;
        }

        Iso9660Entry entry{fullPath, fileLba, fileSize, isDirectory};
        entries_.emplace(fullPath, entry);
        if (entries_.size() > kMaximumEntries) {
            activeDirectories.erase(key);
            error = "A imagem excede o limite de entradas ISO9660 suportado.";
            return false;
        }
        if (isDirectory && !walkDirectory(fileLba, fileSize, fullPath, depth + 1, error)) {
            activeDirectories.erase(key);
            return false;
        }
    }
    activeDirectories.erase(key);
    return true;
}

bool Iso9660Reader::find(std::string_view path, Iso9660Entry& entry) const {
    if (!opened_) return false;
    const auto it = entries_.find(normalizePath(path));
    if (it == entries_.end()) return false;
    entry = it->second;
    return true;
}

bool Iso9660Reader::readFile(const Iso9660Entry& entry, std::vector<uint8_t>& output,
                             size_t maximumBytes, std::string& error) const {
    if (!opened_ || entry.directory || entry.size > maximumBytes ||
        !rangeValid(entry.lba, entry.size)) {
        error = "Arquivo ISO9660 inválido ou maior que o limite permitido.";
        return false;
    }
    output.resize(entry.size);
    size_t count = 0;
    if (!source_.readAt(static_cast<uint64_t>(entry.lba) * kSectorSize,
                        output.data(), output.size(), count)) {
        output.clear();
        error = "Falha ao ler o conteúdo do arquivo ISO9660.";
        return false;
    }
    error.clear();
    return true;
}

bool Iso9660Reader::hashFile(const Iso9660Entry& entry, std::string& digest,
                             std::string& error) const {
    if (!opened_ || entry.directory || !rangeValid(entry.lba, entry.size)) {
        error = "Entrada ISO9660 inválida para cálculo de hash.";
        return false;
    }
    Sha256 hash;
    std::array<uint8_t, 64 * 1024> chunk{};
    uint64_t offset = static_cast<uint64_t>(entry.lba) * kSectorSize;
    uint64_t remaining = entry.size;
    while (remaining > 0) {
        const size_t requested = static_cast<size_t>(
            std::min<uint64_t>(remaining, chunk.size()));
        size_t count = 0;
        if (!source_.readAt(offset, chunk.data(), requested, count)) {
            error = "Falha durante a verificação SHA-256 de " + entry.path + ".";
            return false;
        }
        hash.update(chunk.data(), count);
        offset += count;
        remaining -= count;
    }
    digest = hash.finishHex();
    error.clear();
    return true;
}

std::string Iso9660Reader::volumeId() const { return volumeId_; }
uint32_t Iso9660Reader::volumeSectors() const { return volumeSectors_; }
