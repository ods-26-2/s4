#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "s4/domain/services/frame_source.hpp"
#include "s4/domain/value_objects/p4_frame_metadata.hpp"
#include "s4/infrastructure/gstreamer/software_h264_encoder.hpp"

namespace ods::s4::infrastructure {

// S4.1 na 6c: a camera e de P4. P4 entrega quadros CRUS (NV12) acompanhados dos
// metadados ods.p4.frame, e o S4 codifica com x264enc antes de guardar.
//
//   P4 --(quadro cru + ods.p4.frame)--> submit()
//        appsrc -> [videoconvert] -> x264enc -> h264parse -> appsink
//        --> CapturedFrame { captured_at UTC, session_id de P4, keyframe }
//
// O meio de entrega (shm + canal lateral, socket, biblioteca de P4) ainda sera
// combinado com o SIS-2; esta classe e o ponto em que qualquer um deles se
// liga, porque so pede "um quadro e seus metadados" por chamada.
//
// O encoder nao carrega os metadados de um lado ao outro, entao a fonte guarda
// os de cada quadro numa fila indexada pelo RUNNING TIME que ela mesma atribui
// na entrada (o x264enc desloca o PTS de saida em 1000 h para acomodar DTS
// negativo, mas o running time do segmento continua igual ao da entrada). Na
// saida, o running time do access unit devolve os metadados do quadro de origem.
//
// Troca de sessao (session_id novo em SEEK/LOOP/RESTART): o primeiro quadro da
// sessao nova e codificado como keyframe (evento force-key-unit), para que o
// buffer, zerado pela troca, comece decodificavel em vez de esperar o proximo
// GOP.
class P4RawFrameEncoderSource : public domain::IFrameSource {
public:
    P4RawFrameEncoderSource(
        std::string sourceId,
        domain::RawVideoFormat format,
        SoftwareH264EncoderSettings encoder = {}
    );
    ~P4RawFrameEncoderSource() override;

    P4RawFrameEncoderSource(const P4RawFrameEncoderSource&) = delete;
    P4RawFrameEncoderSource& operator=(const P4RawFrameEncoderSource&) = delete;

    // --- IFrameSource -------------------------------------------------------

    void start(domain::FrameCallback onFrame) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const override;

    // --- entrada de P4 ------------------------------------------------------

    // Um quadro cru e os seus metadados. Thread-safe; os bytes sao copiados, a
    // vista do chamador so precisa valer durante a chamada.
    //
    // Lanca:
    //   * InvalidRawFrameError — outra camera, raw=false, geometria/formato
    //     diferente do combinado ou tamanho de buffer errado;
    //   * FrameSourceError — fonte parada ou pipeline com erro.
    // Se o encoder estiver atrasado (mais de kMaxQueuedFrames na fila), o
    // quadro e descartado e contado em framesDroppedTotal() — nunca em silencio
    // e nunca segurando a thread de P4.
    void submit(const domain::P4FrameMetadata& metadata, const std::uint8_t* data, std::size_t length);

    [[nodiscard]] std::uint64_t framesSubmittedTotal() const noexcept;
    [[nodiscard]] std::uint64_t framesDroppedTotal() const noexcept;
    [[nodiscard]] const std::string& pipelineDescription() const noexcept { return m_pipelineDescription; }
    [[nodiscard]] const domain::RawVideoFormat& format() const noexcept { return m_format; }

    // Quadros crus aceitos na fila do appsrc antes de descartar: ~130 ms a
    // 30 fps, o bastante para absorver um soluco do encoder.
    static constexpr std::size_t kMaxQueuedFrames = 4;

    // Estado opaco (pimpl), publico pelo mesmo motivo do appsink: a callback C
    // do GStreamer precisa alcancar o tipo.
    struct Impl;

private:
    void rejectIfInvalid(const domain::P4FrameMetadata& metadata, std::size_t length) const;

    std::string m_sourceId;
    domain::RawVideoFormat m_format;
    SoftwareH264EncoderSettings m_encoder;
    std::string m_pipelineDescription;
    Impl* m_impl;
};

} // namespace ods::s4::infrastructure
