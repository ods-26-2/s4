#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "s4/domain/services/frame_source.hpp"

namespace ods::s4::infrastructure {

// Cenario gravado: grava e reproduz sequencias de quadros.
//
// Formato propositalmente simples (indice de texto + blob binario) para que um
// teste consiga reproduzir o mesmo cenario quantas vezes quiser, sem camera e
// sem GStreamer. E o "cenario gravado" que o professor exige como evidencia
// minima de teste; quando P6 publicar o formato oficial de replay, estas
// classes viram adaptadores para ele.

// Encaixa-se entre uma fonte e o buffer e persiste cada quadro que passa.
class FrameLogRecorder {
public:
    explicit FrameLogRecorder(const std::string& basePath);
    ~FrameLogRecorder();

    void record(const domain::CapturedFrame& frame);
    void close();

private:
    std::ofstream m_index;
    std::ofstream m_blob;
    std::size_t m_offset{0};
};

// Reproduz um cenario gravado com o MESMO contrato da captura ao vivo.
class RecordedFrameSource : public domain::IFrameSource {
public:
    explicit RecordedFrameSource(
        const std::string& basePath,
        const std::string& sessionOverride = "",
        bool realtime = false
    );
    ~RecordedFrameSource() override;

    void start(domain::FrameCallback onFrame) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const override { return m_running.load(); }

    void waitUntilFinished();

private:
    struct Entry {
        domain::Nanoseconds captureTsNs{0};
        std::size_t offset{0};
        std::size_t length{0};
        bool isKeyframe{false};
        std::string sessionId;
    };

    void replay(domain::FrameCallback onFrame);
    [[nodiscard]] std::vector<Entry> loadIndex() const;

    std::string m_basePath;
    std::string m_sessionOverride;
    bool m_realtime;
    std::vector<std::uint8_t> m_blob;
    // Carregado e validado no construtor: um erro aqui dentro da thread de
    // replay escaparia dela e derrubaria o processo (std::terminate).
    std::vector<Entry> m_entries;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};

} // namespace ods::s4::infrastructure
