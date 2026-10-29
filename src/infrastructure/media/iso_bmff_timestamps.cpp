#include "s4/infrastructure/media/iso_bmff_timestamps.hpp"

#include <cstring>

namespace ods::s4::infrastructure {

namespace {

// Valor sentinela escrito em todos os campos de data/hora. Explicado em
// iso_bmff_timestamps.hpp: o que importa e ser CONSTANTE e obviamente
// nao-real, nao interpretar como algum instante.
//
// 0 significaria 1904-01-01, que e um tempo legitimo e por isso enganoso. Este
// valor e um instante arbitrario bem depois de 1904, que nenhum sistema de
// arquivos de camera produz.
constexpr std::uint32_t kNormalizedTimestamp = 0x2D000000;

constexpr std::size_t kBoxHeaderSize = 8;
constexpr std::size_t kSizeFieldSize = 4;

std::uint32_t readBe32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

void writeBe32(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    bytes[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    bytes[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    bytes[3] = static_cast<std::uint8_t>(value & 0xFF);
}

bool isContainer(const std::uint8_t* type) {
    // Somente estes recursam para filhos. "meta" fica de fora de proposito:
    // ele pode ter 4 bytes de prefixo antes dos filhos, e trata-lo como
    // container de offset fixo desalinharia toda a varredura. Nenhum dos tres
    // boxes com data esta dentro de um meta, entao nada se perde aqui.
    return std::memcmp(type, "moov", 4) == 0 ||
           std::memcmp(type, "trak", 4) == 0 ||
           std::memcmp(type, "mdia", 4) == 0 ||
           std::memcmp(type, "edts", 4) == 0 ||
           std::memcmp(type, "minf", 4) == 0;
}

bool hasDateFields(const std::uint8_t* type) {
    return std::memcmp(type, "mvhd", 4) == 0 ||
           std::memcmp(type, "tkhd", 4) == 0 ||
           std::memcmp(type, "mdhd", 4) == 0;
}

void normalizeBox(std::vector<std::uint8_t>& container, std::size_t start, std::size_t end);

void normalizeBoxes(
    std::vector<std::uint8_t>& container,
    std::size_t start,
    std::size_t end
) {
    std::size_t offset = start;
    while (offset + kBoxHeaderSize <= end) {
        const std::uint32_t size = readBe32(container.data() + offset);
        const std::uint8_t* type = container.data() + offset + kSizeFieldSize;

        // size == 0 significa "ate o fim do arquivo" (so no mdat) e
        // size == 1 significa um size de 64 bits nos 8 bytes seguintes. Nenhum
        // dos dois aparece nos boxes que nos interessam, e aceitar os dois
        // aqui so criaria risco de leitura fora dos limites.
        if (size < kBoxHeaderSize || offset + size > end) {
            return;
        }
        normalizeBox(container, offset, offset + size);
        offset += size;
    }
}

void normalizeBox(std::vector<std::uint8_t>& container, std::size_t start, std::size_t end) {
    const std::uint8_t* type = container.data() + start + kSizeFieldSize;

    if (isContainer(type)) {
        normalizeBoxes(container, start + kBoxHeaderSize, end);
        return;
    }
    if (!hasDateFields(type)) {
        return;
    }

    // Apos o cabecalho de 8 bytes vem version(1) + flags(3). O layout e o
    // mesmo para mvhd/tkhd/mdhd, que e o que permite tratar os tres juntos.
    // A versao 0 usa campos de 32 bits; a versao 1 usa 64. Repousar em
    // 32 bits com um arquivo de versao 1 sobrescreveria metade do campo de
    // tempo seguinte.
    if (start + kBoxHeaderSize + kSizeFieldSize > end) {
        return;
    }
    const std::uint8_t version = container[start + kBoxHeaderSize];
    const std::size_t width = (version == 1) ? 8u : 4u;
    const std::size_t firstField = start + kBoxHeaderSize + kSizeFieldSize;
    const std::size_t secondField = firstField + width;
    if (secondField + width > end) {
        return;
    }

    if (width == 4) {
        writeBe32(container.data() + firstField, kNormalizedTimestamp);
        writeBe32(container.data() + secondField, kNormalizedTimestamp);
    } else {
        // Versao 1: os dois campos de 64 bits sao zerados por completo, e
        // deliberadamente NAO escrito o valor fixo de 32 bits, que seria
        // diferente.
        std::memset(container.data() + firstField, 0, width);
        std::memset(container.data() + secondField, 0, width);
    }
}

} // namespace

void normalize_iso_bmff_timestamps(std::vector<std::uint8_t>& container) {
    if (container.size() < kBoxHeaderSize) {
        return;
    }
    // So mexe num container que comeca com um box de nivel superior plausivel.
    // Qualquer outra coisa e melhor devolvida intacta do que reinterpretada.
    const std::uint8_t* type = container.data() + kSizeFieldSize;
    const bool plausible =
        std::memcmp(type, "ftyp", 4) == 0 ||
        std::memcmp(type, "moov", 4) == 0 ||
        std::memcmp(type, "styp", 4) == 0 ||
        std::memcmp(type, "free", 4) == 0 ||
        std::memcmp(type, "skip", 4) == 0 ||
        std::memcmp(type, "wide", 4) == 0;
    if (!plausible) {
        return;
    }
    normalizeBoxes(container, 0, container.size());
}

} // namespace ods::s4::infrastructure
