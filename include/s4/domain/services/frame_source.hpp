#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::domain {

// Em que relogio esta o captureTsNs de um quadro.
//
//   SourceClock    relogio proprio da fonte: monotonico do GStreamer, fonte
//                  sintetica ou cenario gravado. Nao tem relacao com UTC; o
//                  CaptureClockBridge ancora esse relogio no de parede.
//   CapturedAtUtc  captured_at de ods.p4.frame, em ns desde a epoca Unix (UTC):
//                  a base de tempo oficial de B2. Um evento do S2 (started_at,
//                  captured_at) cai direto no indice, sem conversao.
enum class CaptureTimeBase { SourceClock, CapturedAtUtc };

// Um quadro como chega da fonte, ANTES de entrar no buffer.
//
// Atencao ao par (data, length): e uma VISTA para a memoria do GStreamer,
// valida apenas durante a chamada do callback. Nao ha copia aqui — a unica
// copia da ingestao e a que leva os bytes para a arena.
//
// Diferenca para infrastructure::VideoFrame (usado por S4.2): aquele e o DTO
// da fronteira, dono dos proprios bytes e com relogio de parede; este e o
// quadro cru na borda da captura, com o relogio de captura de P4.
struct CapturedFrame {
    // Instante da captura em P4 (NAO o instante em que o software recebeu o
    // quadro): e a origem do relogio que B2 propaga por todo o sistema.
    Nanoseconds captureTsNs{0};
    const std::uint8_t* data{nullptr};
    std::size_t length{0};
    bool isKeyframe{false};
    // Sessao de execucao ou de replay; muda a cada busca/laco/reinicio de P6.
    // Vazio = a fonte nao conhece a sessao oficial (ex.: shmsrc sem metadados):
    // o quadro herda a ultima sessao anunciada por ods.session.started.
    std::string sessionId;
    CaptureTimeBase timeBase{CaptureTimeBase::SourceClock};
};

using FrameCallback = std::function<void(const CapturedFrame&)>;

// Porta de ENTRADA: quem entrega quadros ao buffer.
//
// Camera ao vivo, replay de P6, cenario gravado e produtor falso implementam o
// MESMO contrato — e exatamente a "paridade de contrato entre captura e
// replay" que o professor exige de P6.
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    virtual void start(FrameCallback onFrame) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;
};

} // namespace ods::s4::domain
