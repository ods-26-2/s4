#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::domain {

// 6c, ods.session.started: modo da sessao que comecou.
enum class SessionMode { Live, Replay };

// 6c, ods.session.started: por que a sessao mudou. SEEK e LOOP indicam que o
// tempo de captura VOLTOU; sem o marcador, o buffer veria esses quadros como
// relogio retrocedendo e os rejeitaria.
enum class SessionChangeReason { Unspecified, Start, Seek, Loop, Restart, Reconnect };

// Marcador de sessao (B2/TRA-2), publicado por P4 e P5 ao iniciar a captura ao
// vivo e por P6 a cada START, SEEK, LOOP e RESTART de replay.
//
// Este objeto e o payload ja interpretado: quem le o envelope do barramento
// (o consumidor B3 do S4.2) monta o SessionStarted e entrega ao buffer. O S4.1
// nao conhece JSON nem Kafka.
struct SessionStarted {
    // session_id do envelope: o id da sessao que comecou.
    std::string sessionId;
    SessionMode mode{SessionMode::Live};
    // started_at, em ns desde a epoca Unix (UTC), na base de tempo de B2.
    Nanoseconds startedAtUtcNs{0};
    // Cameras e microfones que alimentam a sessao.
    std::vector<std::string> sources;
    std::string recordingId;
    std::string previousSessionId;
    SessionChangeReason reason{SessionChangeReason::Unspecified};

    // A sessao vale para esta fonte? Lista vazia e lida como "todas", para nao
    // ignorar um marcador incompleto e misturar duas sessoes num clipe.
    [[nodiscard]] bool concerns(const std::string& sourceId) const {
        return sources.empty() ||
               std::find(sources.begin(), sources.end(), sourceId) != sources.end();
    }

    [[nodiscard]] bool rewindsCaptureClock() const noexcept {
        return reason == SessionChangeReason::Seek || reason == SessionChangeReason::Loop;
    }
};

[[nodiscard]] inline const char* to_string(SessionChangeReason reason) noexcept {
    switch (reason) {
        case SessionChangeReason::Start:
            return "START";
        case SessionChangeReason::Seek:
            return "SEEK";
        case SessionChangeReason::Loop:
            return "LOOP";
        case SessionChangeReason::Restart:
            return "RESTART";
        case SessionChangeReason::Reconnect:
            return "RECONNECT";
        case SessionChangeReason::Unspecified:
            break;
    }
    return "UNSPECIFIED";
}

} // namespace ods::s4::domain
