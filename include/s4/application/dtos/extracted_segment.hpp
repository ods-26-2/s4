#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "s4/domain/value_objects/capture_window.hpp"

namespace ods::s4::application {

// Um quadro JA COPIADO para fora da memoria compartilhada: dono dos proprios
// bytes, e nao uma vista. Depois que o segmento e devolvido, o ring buffer pode
// sobrescrever a arena a vontade sem corromper o trecho.
struct ExtractedFrame {
    std::uint64_t sequence{0};
    domain::Nanoseconds captureTsNs{0};
    bool isKeyframe{false};
    std::vector<std::uint8_t> data;
};

// Trecho pronto para o S4.2 (Binding Evento-Midia) muxar em .mp4.
struct ExtractedSegment {
    domain::CaptureWindow requestedWindow;
    std::string sessionId;
    std::vector<ExtractedFrame> frames;
    bool isTruncatedAtStart{false};
    bool isTruncatedAtEnd{false};

    [[nodiscard]] domain::Nanoseconds firstCaptureTsNs() const { return frames.front().captureTsNs; }
    [[nodiscard]] domain::Nanoseconds lastCaptureTsNs() const { return frames.back().captureTsNs; }

    [[nodiscard]] std::size_t totalBytes() const {
        std::size_t total = 0;
        for (const auto& frame : frames) {
            total += frame.data.size();
        }
        return total;
    }

    // Concatena os quadros na ordem de captura. Para H.264 em Annex-B isso ja e
    // um elementary stream valido, pronto para o muxer.
    [[nodiscard]] std::vector<std::uint8_t> asElementaryStream() const {
        std::vector<std::uint8_t> stream;
        stream.reserve(totalBytes());
        for (const auto& frame : frames) {
            stream.insert(stream.end(), frame.data.begin(), frame.data.end());
        }
        return stream;
    }
};

} // namespace ods::s4::application
