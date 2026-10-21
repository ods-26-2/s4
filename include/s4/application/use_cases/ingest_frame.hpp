#pragma once

#include "s4/domain/entities/ring_buffer.hpp"
#include "s4/domain/repositories/frame_store.hpp"
#include "s4/domain/services/frame_source.hpp"

namespace ods::s4::application {

// Caso de uso: colocar um quadro recem-capturado no buffer.
//
// A ordem das duas operacoes e o que garante a integridade do buffer:
// allocate() decide quais descritores morrem ANTES de os bytes deles serem
// sobrescritos, entao nenhum leitor consegue ver um descritor apontando para
// memoria ja reutilizada.
class IngestFrameUseCase {
public:
    IngestFrameUseCase(domain::RingBuffer& ringBuffer, domain::IFrameStore& frameStore)
        : m_ringBuffer(ringBuffer), m_frameStore(frameStore) {}

    domain::Allocation execute(const domain::CapturedFrame& frame) {
        const domain::Allocation allocation = m_ringBuffer.allocate(
            frame.captureTsNs, frame.length, frame.isKeyframe, frame.sessionId
        );
        m_frameStore.write(allocation.descriptor.offset, frame.data, frame.length);
        return allocation;
    }

private:
    domain::RingBuffer& m_ringBuffer;
    domain::IFrameStore& m_frameStore;
};

} // namespace ods::s4::application
