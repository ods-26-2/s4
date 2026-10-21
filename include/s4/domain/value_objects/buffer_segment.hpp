#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "s4/domain/value_objects/capture_window.hpp"
#include "s4/domain/value_objects/frame_descriptor.hpp"

namespace ods::s4::domain {

// Sequencia DECODIFICAVEL de descritores que cobre a janela pedida.
//
// "frames" sempre comeca em um keyframe. As flags de truncamento avisam se o
// "antes" ou o "depois" do evento ficaram de fora (janela antiga demais, ou
// pos-evento ainda nao capturado) — informacao que o S4.2 registra no
// metadado do clipe, em vez de entregar um trecho silenciosamente incompleto.
struct BufferSegment {
    CaptureWindow requestedWindow;
    std::vector<FrameDescriptor> frames;
    bool isTruncatedAtStart{false};
    bool isTruncatedAtEnd{false};

    [[nodiscard]] Nanoseconds firstCaptureTsNs() const { return frames.front().captureTsNs; }
    [[nodiscard]] Nanoseconds lastCaptureTsNs() const { return frames.back().captureTsNs; }
    [[nodiscard]] const std::string& sessionId() const { return frames.front().sessionId; }

    [[nodiscard]] std::size_t totalBytes() const {
        std::size_t total = 0;
        for (const auto& frame : frames) {
            total += frame.length;
        }
        return total;
    }
};

} // namespace ods::s4::domain
