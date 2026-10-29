// Testes da normalizacao dos campos de data do container ISO-BMFF.
//
// Esta funcao ESCREVE dentro dos bytes do artefato de evidencia, entao um erro
// aqui nao aparece como container invalido: aparece como evidencia adulterada,
// que e o pior tipo de bug do sistema. Cada teste fixa um byte exato.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "tests/ods_check.hpp"

#include "s4/infrastructure/media/iso_bmff_timestamps.hpp"

namespace {

using ods::s4::infrastructure::normalize_iso_bmff_timestamps;

void putBe32(std::vector<std::uint8_t>& at, std::size_t offset, std::uint32_t value) {
    at[offset + 0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    at[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    at[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    at[offset + 3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint32_t getBe32(const std::vector<std::uint8_t>& from, std::size_t offset) {
    return (static_cast<std::uint32_t>(from[offset + 0]) << 24) |
           (static_cast<std::uint32_t>(from[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(from[offset + 2]) << 8) |
           static_cast<std::uint32_t>(from[offset + 3]);
}

void appendBox(
    std::vector<std::uint8_t>& out,
    const char* type,
    const std::vector<std::uint8_t>& payload
) {
    const std::size_t start = out.size();
    const auto size = static_cast<std::uint32_t>(8 + payload.size());
    // O cabecalho precisa existir antes de putBe32 escrever nele.
    out.resize(start + 8);
    putBe32(out, start, size);
    std::memcpy(out.data() + start + 4, type, 4);
    out.insert(out.end(), payload.begin(), payload.end());
}

// Localiza um box pelo tipo em qualquer profundidade. Calcular as posicoes a
// mao exigiria constantes magicas que quebram a cada mudanca no montagem do
// container de teste; buscar o box e a unica forma de o teste continuar
// descrevendo o layout real.
std::size_t findBoxStart(
    const std::vector<std::uint8_t>& bytes,
    std::size_t from,
    std::size_t to,
    const char* type
) {
    std::size_t offset = from;
    while (offset + 8 <= to) {
        const std::uint32_t size = getBe32(bytes, offset);
        if (size < 8 || offset + size > to) {
            return static_cast<std::size_t>(-1);
        }
        if (std::memcmp(bytes.data() + offset + 4, type, 4) == 0) {
            return offset;
        }
        const bool isContainer =
            std::memcmp(bytes.data() + offset + 4, "moov", 4) == 0 ||
            std::memcmp(bytes.data() + offset + 4, "trak", 4) == 0 ||
            std::memcmp(bytes.data() + offset + 4, "mdia", 4) == 0;
        if (isContainer) {
            const std::size_t found = findBoxStart(bytes, offset + 8, offset + size, type);
            if (found != static_cast<std::size_t>(-1)) {
                return found;
            }
        }
        offset += size;
    }
    return static_cast<std::size_t>(-1);
}

// Posicao do creation_time de um box de cabecalho: cabecalho de 8 bytes, mais
// version(1) + flags(3), mais o proprio campo.
std::size_t dateField(const std::vector<std::uint8_t>& bytes, const char* type) {
    const std::size_t start = findBoxStart(bytes, 0, bytes.size(), type);
    ODS_CHECK(start != static_cast<std::size_t>(-1));
    return start + 8 + 4;
}

// Monta um container minimo e crivel: ftyp + moov(mvhd, trak(tkhd, mdia(mdhd)))
// + mdat. E o mesmo esqueleto que o mp4mux escreve para um video de verdade.
struct MiniContainer {
    std::vector<std::uint8_t> bytes;
    std::size_t mdatPayload{0};
};

MiniContainer makeContainer(std::uint32_t createdAt) {
    MiniContainer c;

    appendBox(c.bytes, "ftyp", std::vector<std::uint8_t>(16, 0x11));

    auto mvhd = std::vector<std::uint8_t>(100, 0x00);
    mvhd[0] = 0; // version 0
    putBe32(mvhd, 4, createdAt);   // creation_time
    putBe32(mvhd, 8, createdAt);   // modification_time
    putBe32(mvhd, 12, 1000);       // timescale
    putBe32(mvhd, 16, 20000);      // duration

    auto tkhd = std::vector<std::uint8_t>(84, 0x00);
    tkhd[0] = 0;
    putBe32(tkhd, 4, createdAt);
    putBe32(tkhd, 8, createdAt);

    auto mdhd = std::vector<std::uint8_t>(24, 0x00);
    mdhd[0] = 0;
    putBe32(mdhd, 4, createdAt);
    putBe32(mdhd, 8, createdAt);
    putBe32(mdhd, 12, 30000);      // timescale
    putBe32(mdhd, 16, 1000);       // duration

    std::vector<std::uint8_t> mdia;
    appendBox(mdia, "mdhd", mdhd);

    std::vector<std::uint8_t> trak;
    appendBox(trak, "tkhd", tkhd);
    appendBox(trak, "mdia", mdia);

    std::vector<std::uint8_t> moov;
    appendBox(moov, "mvhd", mvhd);
    appendBox(moov, "trak", trak);
    appendBox(c.bytes, "moov", moov);

    const std::size_t mdatStart = c.bytes.size();
    std::vector<std::uint8_t> samples(64, 0x5A);
    appendBox(c.bytes, "mdat", samples);
    c.mdatPayload = mdatStart + 8;
    return c;
}

void test_zeraOsCamposDeData() {
    MiniContainer c = makeContainer(0x5AE0E63D);
    normalize_iso_bmff_timestamps(c.bytes);

    // Os tres boxes de cabecalho, e os dois campos de cada um.
    for (const char* box : {"mvhd", "tkhd", "mdhd"}) {
        const std::size_t creation = dateField(c.bytes, box);
        ODS_CHECK(getBe32(c.bytes, creation) == 0x2D000000);
        ODS_CHECK(getBe32(c.bytes, creation + 4) == 0x2D000000);
    }
}

void test_naoMexeNasAmostrasNemNoDuracao() {
    const MiniContainer original = makeContainer(0x5AE0E63D);
    MiniContainer c = original;
    normalize_iso_bmff_timestamps(c.bytes);

    // mdat: as amostras H.264 sao a evidencia. Uma alteracao aqui seria
    // adulteracao silenciosa.
    for (std::size_t i = 0; i < 64; ++i) {
        ODS_CHECK(c.bytes[c.mdatPayload + i] == 0x5A);
    }
    // Normalizar e reescrever: nao pode crescer nem encolher o container, senao
    // os deslocamentos do mdat deixariam de valer.
    ODS_CHECK(c.bytes.size() == original.bytes.size());

    // timescale e duration sao metadados de reproducao, nao de provenance.
    // Zera-los tornaria o player calcular duracao zero.
    const std::size_t mvhd = dateField(c.bytes, "mvhd");
    ODS_CHECK(getBe32(c.bytes, mvhd + 8) == 1000);   // timescale
    ODS_CHECK(getBe32(c.bytes, mvhd + 12) == 20000); // duration
    const std::size_t mdhd = dateField(c.bytes, "mdhd");
    ODS_CHECK(getBe32(c.bytes, mdhd + 8) == 30000);   // timescale
    ODS_CHECK(getBe32(c.bytes, mdhd + 12) == 1000);   // duration
}

void test_eIdempotente() {
    MiniContainer c = makeContainer(0x5AE0E63D);
    normalize_iso_bmff_timestamps(c.bytes);
    const std::vector<std::uint8_t> once = c.bytes;

    normalize_iso_bmff_timestamps(c.bytes);
    ODS_CHECK(c.bytes == once);
}

void test_doisContainersDoMesmoVideoFicamIguais() {
    // Este e o contrato que importa: o mesmo footage, extraido em horas
    // diferentes, produz o mesmo sha256.
    MiniContainer a = makeContainer(0x5AE0E63D);
    MiniContainer b = makeContainer(0x5AE0E641); // 4 s depois
    normalize_iso_bmff_timestamps(a.bytes);
    normalize_iso_bmff_timestamps(b.bytes);
    ODS_CHECK(a.bytes == b.bytes);
}

void test_ignoraBufferQueNaoEISOBMFF() {
    // Nao reinterpretar evidencia: se nao ha um box de topo reconhecivel, o
    // certo e nao tocar em nada.
    std::vector<std::uint8_t> garbage = {0x00, 0x01, 0x02, 0x03, 'x', 'y', 'z', 'z',
                                         0xDE, 0xAD, 0xBE, 0xEF};
    const std::vector<std::uint8_t> original = garbage;
    normalize_iso_bmff_timestamps(garbage);
    ODS_CHECK(garbage == original);

    // Muito curto para ter um box sequer.
    std::vector<std::uint8_t> tiny = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y'};
    const std::vector<std::uint8_t> tinyOriginal = tiny;
    normalize_iso_bmff_timestamps(tiny);
    ODS_CHECK(tiny == tinyOriginal);
}

void test_sobreviveATamanhoDeBoxMalformado() {
    // Um box que declara tamanho maior que o buffer nao pode fazer a funcao
    // escrever fora dos limites. Construimos um ftyp legitimo seguido de um
    // moov com tamanho absurdo.
    std::vector<std::uint8_t> buffer;
    appendBox(buffer, "ftyp", std::vector<std::uint8_t>(16, 0x11));
    const std::size_t badStart = buffer.size();
    buffer.resize(badStart + 24, 0xEE);
    putBe32(buffer, badStart, 0x7FFFFFF0u); // tamanho absurdo
    std::memcpy(buffer.data() + badStart + 4, "moov", 4);
    buffer.resize(buffer.size() + 16, 0xEE);

    const std::vector<std::uint8_t> original = buffer;
    normalize_iso_bmff_timestamps(buffer);
    ODS_CHECK(buffer == original);
}

void test_trataVersao1ComCamposDe64Bits() {
    // Versao 1 usa 64 bits para os campos de data. Zerar so 4 bytes deixaria a
    // metade alta intacta e o container continuaria nao-determinismo.
    std::vector<std::uint8_t> mvhd(108, 0x00);
    mvhd[0] = 1; // versao 1
    for (std::size_t i = 4; i < 20; ++i) {
        mvhd[i] = 0xEE; // 64 bits de creation + 64 de modification
    }

    std::vector<std::uint8_t> trak;
    appendBox(trak, "tkhd", mvhd);
    std::vector<std::uint8_t> moov;
    appendBox(moov, "trak", trak);
    std::vector<std::uint8_t> buffer;
    appendBox(buffer, "ftyp", std::vector<std::uint8_t>(8, 0x11));
    appendBox(buffer, "moov", moov);

    normalize_iso_bmff_timestamps(buffer);
    const std::size_t tkhdCreation = dateField(buffer, "tkhd");
    for (std::size_t i = 0; i < 16; ++i) {
        ODS_CHECK(buffer[tkhdCreation + i] == 0x00);
    }
}

} // namespace

int main() {
    test_zeraOsCamposDeData();
    test_naoMexeNasAmostrasNemNoDuracao();
    test_eIdempotente();
    test_doisContainersDoMesmoVideoFicamIguais();
    test_ignoraBufferQueNaoEISOBMFF();
    test_sobreviveATamanhoDeBoxMalformado();
    test_trataVersao1ComCamposDe64Bits();
    return 0;
}
