#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "s4/domain/services/frame_source.hpp"

namespace ods::s4::infrastructure {

struct SyntheticSourceOptions {
    double fps{30.0};
    std::size_t gop{30};
    std::size_t keyframeBytes{60000};
    std::size_t deltaBytes{12000};
    std::string sessionId{"synthetic-session-1"};
    domain::Nanoseconds startTsNs{0};
    // realtime=false gera o mais rapido possivel (testes); true respeita o fps.
    bool realtime{true};
    // 0 = sem limite; >0 encerra a thread apos N quadros.
    std::size_t maxFrames{0};
};

// Produtor falso de quadros, para testes e demonstracoes sem camera.
//
// Imita a forma de um stream H.264: um keyframe (I) grande a cada "gop"
// quadros e quadros P menores entre eles, com timestamps espacados por 1/fps.
// O conteudo e deterministico, entao um teste consegue verificar byte a byte
// que o que saiu do buffer e exatamente o que entrou. E o "produtor falso" que
// o professor exige como evidencia minima de teste.
class SyntheticFrameSource : public domain::IFrameSource {
public:
    explicit SyntheticFrameSource(SyntheticSourceOptions options);
    ~SyntheticFrameSource() override;

    void start(domain::FrameCallback onFrame) override;
    void stop() override;
    [[nodiscard]] bool isRunning() const override { return m_running.load(); }

    // Bloqueia ate maxFrames serem entregues (util em testes).
    void waitUntilFinished();

    // Quadro deterministico de indice "index"; publico para que o teste possa
    // comparar o que saiu do buffer com o que deveria ter entrado.
    [[nodiscard]] std::vector<std::uint8_t> payloadAt(std::size_t index) const;
    [[nodiscard]] domain::Nanoseconds timestampAt(std::size_t index) const;
    [[nodiscard]] bool isKeyframeAt(std::size_t index) const;

private:
    void produce(domain::FrameCallback onFrame);

    SyntheticSourceOptions m_options;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};

} // namespace ods::s4::infrastructure
