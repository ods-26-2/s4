#include "s4/domain/entities/ring_buffer.hpp"

#include <string>

namespace ods::s4::domain {

RingBuffer::RingBuffer(BufferCapacity capacity) : m_capacity(capacity.totalBytes()) {}

// ------------------------------------------------------------------ escrita

Allocation RingBuffer::allocate(
    Nanoseconds captureTsNs,
    std::size_t length,
    bool isKeyframe,
    const std::string& sessionId
) {
    rejectIfTooLarge(length);
    const bool sessionRestarted = switchSessionIfNeeded(sessionId);
    rejectIfClockWentBackwards(captureTsNs);
    const std::size_t offset = offsetFor(length);

    Allocation allocation;
    allocation.evicted = evictOverlapping(offset, length);
    allocation.descriptor = registerFrame(captureTsNs, offset, length, isKeyframe, sessionId);
    allocation.sessionRestarted = sessionRestarted;
    return allocation;
}

void RingBuffer::rejectIfTooLarge(std::size_t length) const {
    if (length == 0 || length > m_capacity) {
        throw FrameTooLargeError(
            "Frame of " + std::to_string(length) + " bytes does not fit in an arena of " +
            std::to_string(m_capacity) + " bytes"
        );
    }
}

// Sessao nova (replay, busca, reinicio) invalida o buffer inteiro: B2 exige que
// busca/laco/reinicio gerem sessao nova, e misturar quadros de duas sessoes
// produziria um clipe com salto de tempo invisivel.
bool RingBuffer::switchSessionIfNeeded(const std::string& sessionId) {
    if (!m_hasSession) {
        m_hasSession = true;
        m_sessionId = sessionId;
        return false;
    }
    if (m_sessionId == sessionId) {
        return false;
    }
    m_evictedTotal += m_frames.size();
    m_frames.clear();
    m_writeOffset = 0;
    m_sessionId = sessionId;
    return true;
}

void RingBuffer::rejectIfClockWentBackwards(Nanoseconds captureTsNs) const {
    if (!m_frames.empty() && captureTsNs < m_frames.back().captureTsNs) {
        throw NonMonotonicTimestampError(
            "captureTs " + std::to_string(captureTsNs) + " < last " +
            std::to_string(m_frames.back().captureTsNs) + " in session " + m_sessionId
        );
    }
}

std::size_t RingBuffer::offsetFor(std::size_t length) {
    if (m_writeOffset + length <= m_capacity) {
        return m_writeOffset;
    }
    discardFramesBeyondWritePointer();
    return 0;
}

// Ao dar a volta, os quadros da volta anterior que ainda estavam A FRENTE do
// ponteiro de escrita passam a ser os mais antigos e sao descartados de uma
// vez. E isso que mantem a fila ordenada por offset a partir do zero apos cada
// volta — condicao para que a expulsao so precise olhar o inicio dela.
void RingBuffer::discardFramesBeyondWritePointer() {
    while (!m_frames.empty() && m_frames.front().offset >= m_writeOffset) {
        m_frames.pop_front();
        ++m_evictedTotal;
    }
}

std::vector<FrameDescriptor> RingBuffer::evictOverlapping(std::size_t offset, std::size_t length) {
    std::vector<FrameDescriptor> evicted;
    while (!m_frames.empty() && m_frames.front().occupies(offset, offset + length)) {
        evicted.push_back(m_frames.front());
        m_frames.pop_front();
        ++m_evictedTotal;
    }
    return evicted;
}

FrameDescriptor RingBuffer::registerFrame(
    Nanoseconds captureTsNs,
    std::size_t offset,
    std::size_t length,
    bool isKeyframe,
    const std::string& sessionId
) {
    FrameDescriptor descriptor;
    descriptor.sequence = m_nextSequence;
    descriptor.captureTsNs = captureTsNs;
    descriptor.offset = offset;
    descriptor.length = length;
    descriptor.isKeyframe = isKeyframe;
    descriptor.sessionId = sessionId;

    m_frames.push_back(descriptor);
    m_writeOffset = offset + length;
    ++m_nextSequence;
    ++m_ingestedTotal;
    return descriptor;
}

// ------------------------------------------------------------------ leitura

BufferSegment RingBuffer::segmentFor(const CaptureWindow& window) const {
    rejectIfWindowAbsent(m_frames, window);
    const std::size_t startIndex = decodableStartIndex(m_frames, window);
    const std::size_t endIndex = lastIndexWithin(m_frames, window);

    // O inicio e truncado quando o trecho ENTREGUE comeca depois do pedido — e
    // nao so quando o buffer inteiro comeca depois: se o keyframe anterior ja
    // foi expulso, o trecho pula para o primeiro keyframe dentro da janela e
    // perde parte do "antes" mesmo havendo quadros P mais antigos no buffer.
    BufferSegment segment{
        window,
        {},
        m_frames[startIndex].captureTsNs > window.startNs(),
        m_frames.back().captureTsNs < window.endNs()
    };
    segment.frames.assign(
        m_frames.begin() + static_cast<std::ptrdiff_t>(startIndex),
        m_frames.begin() + static_cast<std::ptrdiff_t>(endIndex) + 1
    );
    return segment;
}

void RingBuffer::rejectIfWindowAbsent(
    const std::deque<FrameDescriptor>& frames,
    const CaptureWindow& window
) {
    for (const auto& frame : frames) {
        if (window.contains(frame.captureTsNs)) {
            return;
        }
    }
    throw WindowNotInBufferError(
        "No frame in [" + std::to_string(window.startNs()) + ", " +
        std::to_string(window.endNs()) + "] is in the buffer"
    );
}

// Ultimo keyframe ANTES (ou em) startNs; se ele ja foi expulso, o primeiro
// keyframe DENTRO da janela. Um quadro P/B sem o keyframe que o antecede e lixo
// para o decoder, entao o trecho precisa comecar em um keyframe mesmo que isso
// puxe alguns quadros a mais antes do inicio pedido.
std::size_t RingBuffer::decodableStartIndex(
    const std::deque<FrameDescriptor>& frames,
    const CaptureWindow& window
) {
    bool foundBefore = false;
    std::size_t lastKeyframeBefore = 0;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].isKeyframe && frames[index].captureTsNs <= window.startNs()) {
            lastKeyframeBefore = index;
            foundBefore = true;
        }
    }
    if (foundBefore) {
        return lastKeyframeBefore;
    }
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].isKeyframe && window.contains(frames[index].captureTsNs)) {
            return index;
        }
    }
    throw NoDecodableStartError();
}

std::size_t RingBuffer::lastIndexWithin(
    const std::deque<FrameDescriptor>& frames,
    const CaptureWindow& window
) {
    std::size_t last = 0;
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].captureTsNs <= window.endNs()) {
            last = index;
        }
    }
    return last;
}

// ----------------------------------------------------------------- inspecao

Nanoseconds RingBuffer::newestCaptureTsNs() const {
    return m_frames.empty() ? 0 : m_frames.back().captureTsNs;
}

Nanoseconds RingBuffer::oldestCaptureTsNs() const {
    return m_frames.empty() ? 0 : m_frames.front().captureTsNs;
}

BufferStats RingBuffer::stats() const {
    BufferStats stats;
    stats.capacityBytes = m_capacity;
    stats.framesStored = m_frames.size();
    stats.framesIngestedTotal = m_ingestedTotal;
    stats.framesEvictedTotal = m_evictedTotal;
    stats.hasFrames = !m_frames.empty();
    stats.oldestCaptureTsNs = oldestCaptureTsNs();
    stats.newestCaptureTsNs = newestCaptureTsNs();
    stats.sessionId = m_sessionId;
    for (const auto& frame : m_frames) {
        stats.bytesUsed += frame.length;
    }
    return stats;
}

} // namespace ods::s4::domain
