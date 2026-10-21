#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "s4/application/dtos/extracted_segment.hpp"
#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::application {

// Erro do muxing: o segmento nao virou um container valido.
// Herda de ODSBaseException (como os demais erros do componente) para que a
// camada HTTP e os testes possam tratar a familia toda com um unico catch.
class MediaMuxError : public domain::ODSBaseException {
public:
    explicit MediaMuxError(const std::string& what) : domain::ODSBaseException(what) {}
};

// Porta de muxing: transforma um trecho H.264 Annex-B em um container.
//
// Por que o muxer NAO pode ser "concatenar e renomear para .mp4"?
//
// Um .mp4 de verdade e um container ISO-BMFF: precisa de cabecalho ftyp,
// tabelas de sample (stsz/stco/stss), e o codec emLength-prefixed NAL units
// dentro de amostras length-prefixed. A concatenacao de Annex-B e um
// elementary stream H.264 — o mesmo conteudo, mas com o container errado. Um
// player que abra o arquivo rejeita; um validador de MP4 reprova; e o
// hash descreve um formato que ninguem promised.
//
// A porta devolve BYTES, e nao caminho de arquivo: assim o hash e calculado
// sobre exatamente o que foi escrito em disco, sem uma segunda leitura e sem
// risco de o arquivo mudar entre o write e o hash.
class IMediaMuxer {
public:
    virtual ~IMediaMuxer() = default;

    // Muxa o segmento e devolve o container completo.
    //
    // Os quadros chegam em ordem de captura (= ordem de decodificacao, porque
    // o buffer carimba o DTS) e em Annex-B com SPS/PPS repetidos antes de cada
    // keyframe, que sao os dois pre-requisitos para um container valido.
    [[nodiscard]] virtual std::vector<std::uint8_t> mux(const ExtractedSegment& segment) = 0;

    // Nome do container produzido, usado no descritor do clipe.
    [[nodiscard]] virtual const char* containerName() const = 0;
};

} // namespace ods::s4::application
