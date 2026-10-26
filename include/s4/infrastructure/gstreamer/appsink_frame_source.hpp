#pragma once

#include <cstddef>
#include <string>

#include "s4/domain/services/frame_source.hpp"
#include "s4/domain/value_objects/p4_frame_metadata.hpp"
#include "s4/infrastructure/gstreamer/software_h264_encoder.hpp"

namespace ods::s4::infrastructure {

// Camera CSI aberta pelo proprio S4. Na 6c a camera e de P4: este caminho fica
// para bancada (medir o encoder na placa sem depender de P4) e diagnostico.
struct CsiCameraOptions {
    int sensorId{0};
    int width{1920};
    int height{1080};
    int fps{30};
    // GOP e bitrate moram no encoder: keyframeInterval = fps da 1 keyframe/s.
    SoftwareH264EncoderSettings encoder;
};

// IFrameSource sobre GStreamer appsink.
//
// O pipeline termina em um appsink; cada buffer H.264 (um access unit por
// buffer, com SPS/PPS repetidos em todo keyframe) vira um CapturedFrame com o
// tempo de captura e a flag de keyframe.
//
// Tempo de captura: se o buffer traz um GstReferenceTimestampMeta com a
// referencia "timestamp/x-unix", e esse o captured_at oficial (UTC) — o meta
// atravessa x264enc e h264parse, entao quem carimba antes do encoder (P4/P6)
// chega intacto ao indice. Sem o meta, vale o relogio do pipeline.
//
// sessionId vazio = a fonte nao conhece a sessao oficial; os quadros herdam a
// anunciada por ods.session.started (ver RingBufferRAMFacade).
//
// Compilado com o GStreamer real apenas quando o CMake o encontra
// (ODS_S4_WITH_GSTREAMER); sem ele, os pipelines continuam disponiveis como
// texto e start() falha com FrameSourceError — assim o restante do S4 segue
// compilavel e testavel em maquinas sem a biblioteca.
class GStreamerAppsinkFrameSource : public domain::IFrameSource {
public:
    GStreamerAppsinkFrameSource(std::string pipelineDescription, std::string sessionId);
    ~GStreamerAppsinkFrameSource() override;

    // --- pipelines prontos --------------------------------------------------

    // Camera CSI (IMX477/IMX519) codificada por SOFTWARE (x264enc): a Orin
    // Nano nao tem NVENC. nvvidconv so tira o quadro da memoria NVMM.
    [[nodiscard]] static std::string jetsonCsiH264Pipeline(const CsiCameraOptions& options);
    // Stream H.264 publicado via shmsink (P4 nao codifica na 6c; mantido para
    // fontes que ja entreguem H.264).
    [[nodiscard]] static std::string shmH264Pipeline(const std::string& socketPath);
    // 6c: P4 entrega quadros CRUS (NV12) via shmsink; o S4 codifica. O shm nao
    // carrega os metadados ods.p4.frame: tempo do pipeline e sessao herdada do
    // marcador. Com metadados por quadro, use P4RawFrameEncoderSource.
    [[nodiscard]] static std::string p4RawShmPipeline(
        const std::string& socketPath,
        const domain::RawVideoFormat& format,
        const SoftwareH264EncoderSettings& encoder = {}
    );
    // Replay de um .mp4 gravado (papel que P6 assumira com contrato proprio).
    [[nodiscard]] static std::string fileReplayH264Pipeline(const std::string& path);
    // Padrao de teste + x264 (CPU): roda em qualquer PC com GStreamer.
    [[nodiscard]] static std::string testPatternH264Pipeline(
        int width = 1280, int height = 720, int fps = 30, int bitrateKbps = 2000, int gop = 30
    );

    // --- IFrameSource -------------------------------------------------------

    void start(domain::FrameCallback onFrame) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const override;

    [[nodiscard]] const std::string& pipelineDescription() const noexcept {
        return m_pipelineDescription;
    }

    // Estado opaco do pimpl: esconde gst/*.h de quem inclui este header, e a
    // definicao continua visivel apenas no .cpp. O NOME e publico de proposito
    // porque a callback C do appsink (on_new_sample, funcao livre no .cpp) e
    // as funcoes de callback precisam alcancar este tipo.
    struct Impl;

private:
    Impl* m_impl;
    std::string m_pipelineDescription;
    std::string m_sessionId;
};

} // namespace ods::s4::infrastructure
