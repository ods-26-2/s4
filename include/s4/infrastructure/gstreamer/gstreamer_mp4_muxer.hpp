#pragma once

#include <string>

#include "s4/application/services/media_muxer.hpp"

namespace ods::s4::infrastructure {

// S4.4 — Muxer MP4 real sobre GStreamer.
//
// Pipeline: appsrc (Annex-B, byte-stream, alinhado em access unit)
//        -> h264parse config-interval=-1
//        -> mp4mux faststart=true
//        -> appsink
//
// Decisoes que importam:
//
//   * h264parse config-interval=-1: repete SPS/PPS antes de TODO keyframe.
//     O ring buffer entrega os quadros a partir de um keyframe, mas um
//     extrator de MP4 NAO pode assumir que o SPS/PPS esteja no primeiro sample
//     do arquivo; repetir os parametros e o que torna o container autocontido
//     e cada trecho independente.
//
//   * appsrc sem width/height/fps fixos: o caps de entrada declara apenas
//     stream-format e alignment, e o h264parse deriva as dimensoes reais do
//     SPS. Fixar aquieria com qualquer camera: o RingBufferConfig carrega
//     width/height de configuracao, que e um palpite, nao a verdade do
//     stream. A 640x360 do arquivo de teste nao e a 320x240 da config.
//
//   * Os quadros sao empurrados na ordem em que chegaram, que e a ordem de
//     captura (= ordem de decodificacao, porque o buffer carimba o DTS). O
//     mp4mux precisa de ordem de decodificacao para escrever as tabelas de
//     amostra; ordenar por PTS aqui seria errado e produziria duracao
//     incorreta.
//
//   * Normalizacao dos timestamps do container (ligada por padrao): o mp4mux
//     grava a HORA DO MUXING nos boxes de cabecalho, o que torna dois clips
//     identicos byte-a-byte diferentes e quebra a repetibilidade do hash
//     SHA-256 que o evidencia precisa ter. Ver a discussao em
//     infrastructure/media/iso_bmff_timestamps.hpp antes de desligar.
//
// Sem GStreamer (ODS_S4_WITH_GSTREAMER desligado) a classe ainda compila e
// mux() falha com MediaMuxError, entao o restante do S4 continua testavel.
class GStreamerMp4Muxer : public application::IMediaMuxer {
public:
    // normalizeTimestamps=true zera os campos de data/hora do container
    // (mvhd/tkhd/mdhd) para que o mesmo footage produza sempre o mesmo
    // sha256. Passe false apenas se o cabecalho precisar preservar a hora
    // real do muxing.
    explicit GStreamerMp4Muxer(bool normalizeTimestamps = true);
    ~GStreamerMp4Muxer() override;

    GStreamerMp4Muxer(const GStreamerMp4Muxer&) = delete;
    GStreamerMp4Muxer& operator=(const GStreamerMp4Muxer&) = delete;

    [[nodiscard]] std::vector<std::uint8_t> mux(
        const application::ExtractedSegment& segment
    ) override;

    [[nodiscard]] const char* containerName() const override { return "mp4"; }

    // Descricao do pipeline, exposta para os testes poderem verificar o que
    // sera montado sem precisar instanciar GStreamer.
    [[nodiscard]] static std::string pipelineDescription();

private:
    bool m_normalizeTimestamps;
    struct Impl;
    Impl* m_impl;
};

} // namespace ods::s4::infrastructure
