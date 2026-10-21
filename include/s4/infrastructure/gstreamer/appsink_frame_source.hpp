#pragma once

#include <cstddef>
#include <string>

#include "s4/domain/services/frame_source.hpp"

namespace ods::s4::infrastructure {

struct CsiCameraOptions {
    int sensorId{0};
    int width{1920};
    int height{1080};
    int fps{30};
    std::size_t bitrateBps{4000000};
    int gop{30};
};

// IFrameSource sobre GStreamer appsink.
//
// O pipeline termina em um appsink; cada buffer H.264 (um access unit por
// buffer, com SPS/PPS repetidos em todo keyframe) vira um CapturedFrame com o
// tempo de captura e a flag de keyframe.
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

    // Camera CSI (IMX477/IMX519) codificada pelo NVENC da Jetson.
    [[nodiscard]] static std::string jetsonCsiH264Pipeline(const CsiCameraOptions& options);
    // Stream H.264 que P4 publica via shmsink em /dev/shm.
    [[nodiscard]] static std::string shmH264Pipeline(const std::string& socketPath);
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
