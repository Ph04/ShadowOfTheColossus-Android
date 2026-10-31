#include "disc_inspector.h"
#include "iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
constexpr int64_t kExpectedDiscSize = 4635918336ll;
constexpr uint32_t kExpectedVolumeSectors = 2263632u;
constexpr const char* kSerial = "SCES-53326";
constexpr const char* kVersion = "1.00";
constexpr const char* kBootPath = "SCES_533.26";
constexpr const char* kBootSha = "851f0c4f48b0b4bb61e89fb7e97d4e9b4ac4c2ea0ff143d1d95e18337ea8b57c";
constexpr uint32_t kBootSize = 229888u;

struct ExpectedFile {
    const char* path;
    uint32_t size;
    const char* sha256;
    bool xff;
};

constexpr ExpectedFile kModules[] = {
    {"KERNEL.XFF", 419668u, "d83a2d4d7631ed740946849b1b4ba32620891fc41817bb16175fedb9f9cd6463", true},
    {"MANAGER.XFF", 1732264u, "2de4d80eed7ff6c89b34b587ee1ced8792921e2a7c80d7e92a4fbb1481f1461a", true},
    {"GAMECORE.XFF", 2157144u, "aa5a72b239b36da0bfc9f435bf31441057b4f4b8a01405b94b0e3b212fd242e0", true}
};

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(),
        [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

bool readEntry(Iso9660Reader& iso, const char* path, Iso9660Entry& entry,
               std::string& error) {
    if (!iso.find(path, entry) || entry.directory) {
        error = std::string("Arquivo obrigatório ausente no disco: ") + path;
        return false;
    }
    return true;
}

bool verifyHash(Iso9660Reader& iso, const ExpectedFile& expected,
                std::string& error) {
    Iso9660Entry entry;
    if (!readEntry(iso, expected.path, entry, error)) return false;
    if (entry.size != expected.size) {
        error = std::string(expected.path) + ": tamanho incorreto (") +
                std::to_string(entry.size) + " bytes; esperado " +
                std::to_string(expected.size) + ").";
        return false;
    }
    if (expected.xff) {
        std::vector<uint8_t> header;
        if (!iso.readFile(entry, header, expected.size, error)) return false;
        if (header.size() < 4 || header[0] != 'x' || header[1] != 'f' ||
            header[2] != 'f' || header[3] != '2') {
            error = std::string(expected.path) + ": cabeçalho xff2 ausente.";
            return false;
        }
    }
    std::string digest;
    if (!iso.hashFile(entry, digest, error)) return false;
    if (digest != expected.sha256) {
        error = std::string(expected.path) + ": SHA-256 não corresponde ao perfil SCES-53326 v1.00.";
        return false;
    }
    return true;
}

bool parseBootConfig(const std::vector<uint8_t>& bytes,
                     std::string& boot, std::string& version, std::string& error) {
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos) continue;
        const std::string key = upper(trim(line.substr(0, equal)));
        const std::string value = trim(line.substr(equal + 1));
        if (key == "BOOT2" || (key == "BOOT" && boot.empty())) {
            const size_t colon = value.find(':');
            std::string path = colon == std::string::npos ? value : value.substr(colon + 1);
            std::replace(path.begin(), path.end(), '\\\\', '/');
            const size_t slash = path.find_last_of('/');
            if (slash != std::string::npos) path.erase(0, slash + 1);
            const size_t semicolon = path.find(';');
            if (semicolon != std::string::npos) path.resize(semicolon);
            boot = upper(trim(path));
        } else if (key == "VER") {
            version = trim(value);
        }
    }
    if (boot.empty()) {
        error = "SYSTEM.CNF não contém BOOT2/BOOT.";
        return false;
    }
    if (version.empty()) {
        error = "SYSTEM.CNF não contém VER.";
        return false;
    }
    return true;
}
}

bool inspectShadowOfTheColossusDisc(const IsoSource& source,
                                    DiscInspection& inspection,
                                    std::string& error) {
    inspection = {};
    if (source.size() != kExpectedDiscSize) {
        error = "Tamanho da imagem incompatível. Este port inicial exige a imagem PAL SCES-53326 v1.00 (4.635.918.336 bytes).";
        return false;
    }

    Iso9660Reader iso(source);
    if (!iso.open(error)) return false;
    if (iso.volumeSectors() != kExpectedVolumeSectors) {
        error = "O volume ISO9660 não possui a quantidade de setores esperada para SCES-53326 v1.00.";
        return false;
    }

    Iso9660Entry systemEntry;
    if (!readEntry(iso, "SYSTEM.CNF", systemEntry, error)) return false;
    std::vector<uint8_t> systemCnf;
    if (!iso.readFile(systemEntry, systemCnf, 64u * 1024u, error)) return false;

    std::string boot, version;
    if (!parseBootConfig(systemCnf, boot, version, error)) return false;
    if (boot != kBootPath || version != kVersion) {
        error = std::string("SYSTEM.CNF identifica BOOT=") + boot + " e VER=" + version +
                "; esperado " + kBootPath + " e versão " + kVersion + ".";
        return false;
    }

    Iso9660Entry bootEntry;
    if (!readEntry(iso, kBootPath, bootEntry, error)) return false;
    if (bootEntry.size != kBootSize) {
        error = "O executável de boot possui tamanho diferente do perfil validado.";
        return false;
    }
    std::vector<uint8_t> elfHeader;
    if (!iso.readFile(bootEntry, elfHeader, kBootSize, error)) return false;
    if (elfHeader.size() < 52 || elfHeader[0] != 0x7f || elfHeader[1] != 'E' ||
        elfHeader[2] != 'L' || elfHeader[3] != 'F' || elfHeader[4] != 1 ||
        elfHeader[5] != 1 || elfHeader[18] != 8 || elfHeader[19] != 0) {
        error = "O executável de boot não é ELF32 little-endian para MIPS.";
        return false;
    }
    const uint32_t entryPoint = static_cast<uint32_t>(elfHeader[24]) |
        (static_cast<uint32_t>(elfHeader[25]) << 8u) |
        (static_cast<uint32_t>(elfHeader[26]) << 16u) |
        (static_cast<uint32_t>(elfHeader[27]) << 24u);
    if (entryPoint != 0x00100010u) {
        error = "O endereço de entrada do ELF não corresponde ao executável SCES-53326 v1.00.";
        return false;
    }
    std::string bootHash;
    if (!iso.hashFile(bootEntry, bootHash, error)) return false;
    if (bootHash != kBootSha) {
        error = "SHA-256 do executável de boot não corresponde ao perfil SCES-53326 v1.00.";
        return false;
    }

    for (const auto& module : kModules) {
        if (!verifyHash(iso, module, error)) return false;
    }

    inspection.supported = true;
    inspection.serial = kSerial;
    inspection.version = kVersion;
    inspection.bootExecutable = kBootPath;
    inspection.summary = "Disco validado: Shadow of the Colossus PAL " + inspection.serial +
        " v" + inspection.version + ". ELF de boot e os três módulos XFF conferem com os hashes SHA-256.";
    error.clear();
    return true;
}
