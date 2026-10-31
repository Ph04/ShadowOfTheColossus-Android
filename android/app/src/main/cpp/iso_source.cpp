#include "iso_source.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int64_t kSectorSize = 2048;
constexpr int64_t kMinimumIsoSize = 17 * kSectorSize;
}

IsoSource::~IsoSource() {
    close();
}

bool IsoSource::attach(int ownedFd, int64_t baseOffset, int64_t length, std::string& error) {
    close();

    if (ownedFd < 0) {
        error = "Descritor de arquivo inválido.";
        return false;
    }

    auto reject = [&](const std::string& reason) {
        ::close(ownedFd);
        error = reason;
        return false;
    };

    if (baseOffset < 0 || length < -1) {
        return reject("O provedor retornou offsets inválidos.");
    }

    struct stat info {};
    if (fstat(ownedFd, &info) != 0) {
        return reject(std::string("Não foi possível consultar a imagem: ") + std::strerror(errno));
    }

    if (length < 0) {
        if (info.st_size <= baseOffset) {
            return reject("O tamanho da imagem não pôde ser determinado.");
        }
        length = static_cast<int64_t>(info.st_size) - baseOffset;
    }

    if (length < kMinimumIsoSize) {
        return reject("O arquivo é pequeno demais para uma imagem de disco PS2 válida.");
    }

    if (info.st_size > 0 &&
        (baseOffset > static_cast<int64_t>(info.st_size) ||
         length > static_cast<int64_t>(info.st_size) - baseOffset)) {
        return reject("O intervalo informado pelo provedor ultrapassa o tamanho do arquivo.");
    }

    fd_ = ownedFd;
    baseOffset_ = baseOffset;
    length_ = length;
    error.clear();
    return true;
}

void IsoSource::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    baseOffset_ = 0;
    length_ = 0;
}

bool IsoSource::isOpen() const {
    return fd_ >= 0;
}

int64_t IsoSource::size() const {
    return length_;
}

bool IsoSource::readAt(uint64_t offset, void* destination, size_t bytes, size_t& bytesRead) const {
    bytesRead = 0;
    if (fd_ < 0 || destination == nullptr || offset > static_cast<uint64_t>(length_)) {
        return false;
    }

    const uint64_t available = static_cast<uint64_t>(length_) - offset;
    const size_t requested = bytes < available ? bytes : static_cast<size_t>(available);
    auto* output = static_cast<unsigned char*>(destination);

    while (bytesRead < requested) {
        const off_t absolute = static_cast<off_t>(baseOffset_ + static_cast<int64_t>(offset) +
                                                  static_cast<int64_t>(bytesRead));
        const ssize_t count = pread(fd_, output + bytesRead, requested - bytesRead, absolute);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytesRead += static_cast<size_t>(count);
    }
    return bytesRead == bytes;
}

std::string IsoSource::description() const {
    if (fd_ < 0) {
        return "ISO: nenhuma imagem selecionada.";
    }
    std::ostringstream text;
    text << "Imagem selecionada: " << (length_ / (1024 * 1024))
         << " MiB | leitura aleatória via SAF ativa.";
    return text.str();
}
