#pragma once

#include "s4/application/dtos/extracted_segment.hpp"
#include "s4/domain/entities/ring_buffer.hpp"
#include "s4/domain/repositories/frame_store.hpp"
#include "s4/domain/value_objects/buffer_segment.hpp"

namespace ods::s4::application {

// Caso de uso: copiar para fora do buffer o trecho que cobre uma janela.
//
// Consulta o indice (dominio) e materializa os bytes de cada descritor
// (infraestrutura). A copia e intencional: ver ExtractedFrame.
class ExtractCaptureWindowUseCase {
public:
    ExtractCaptureWindowUseCase(
        const domain::RingBuffer& ringBuffer,
        const domain::IFrameStore& frameStore
    ) : m_ringBuffer(ringBuffer), m_frameStore(frameStore) {}

    [[nodiscard]] ExtractedSegment execute(const domain::CaptureWindow& window) const {
        const domain::BufferSegment segment = m_ringBuffer.segmentFor(window);
        ExtractedSegment extracted{
            window,
            segment.sessionId(),
            {},
            segment.isTruncatedAtStart,
            segment.isTruncatedAtEnd
        };
        extracted.frames.reserve(segment.frames.size());
        for (const auto& descriptor : segment.frames) {
            extracted.frames.push_back(materialize(descriptor));
        }
        return extracted;
    }

private:
    [[nodiscard]] ExtractedFrame materialize(const domain::FrameDescriptor& descriptor) const {
        ExtractedFrame frame;
        frame.sequence = descriptor.sequence;
        frame.captureTsNs = descriptor.captureTsNs;
        frame.isKeyframe = descriptor.isKeyframe;
        frame.data = m_frameStore.read(descriptor.offset, descriptor.length);
        return frame;
    }

    const domain::RingBuffer& m_ringBuffer;
    const domain::IFrameStore& m_frameStore;
};

} // namespace ods::s4::application
