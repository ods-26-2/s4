#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "s4/domain/value_objects/buffer_capacity.hpp"
#include "s4/domain/value_objects/buffer_segment.hpp"
#include "s4/domain/value_objects/capture_window.hpp"
#include "s4/domain/value_objects/frame_descriptor.hpp"

namespace ods::s4::domain {

// Resultado de allocate(): onde escrever e o que morreu nessa escrita.
struct Allocation {
    FrameDescriptor descriptor;
    std::vector<FrameDescriptor> evicted;
    bool sessionRestarted{false};
};

// Numeros para observabilidade (B2) e para calibrar a janela em campo.
struct BufferStats {
    std::size_t capacityBytes{0};
    std::size_t bytesUsed{0};
    std::size_t framesStored{0};
    std::uint64_t framesIngestedTotal{0};
    std::uint64_t framesEvictedTotal{0};
    bool hasFrames{false};
    Nanoseconds oldestCaptureTsNs{0};
    Nanoseconds newestCaptureTsNs{0};
    std::string sessionId;

    // Quantos segundos de video o buffer esta realmente segurando agora.
    [[nodiscard]] double spanSeconds() const noexcept {
        if (!hasFrames) {
            return 0.0;
        }
        return static_cast<double>(newestCaptureTsNs - oldestCaptureTsNs) / kNanosecondsPerSecond;
    }

    [[nodiscard]] double fillRatio() const noexcept {
        return static_cast<double>(bytesUsed) / static_cast<double>(capacityBytes);
    }
};

// Entidade RingBuffer: o indice circular de quadros.
//
// Esta classe NAO toca bytes. Ela decide tres coisas:
//   1. ONDE cada quadro novo sera escrito na arena circular;
//   2. QUAIS quadros antigos deixam de existir por causa dessa escrita;
//   3. QUAIS quadros respondem a uma janela de tempo, de forma decodificavel.
// Copiar bytes e responsabilidade da infraestrutura (IFrameStore), o que
// mantem toda a logica delicada testavel sem sistema operacional no caminho.
//
// Invariantes mantidos por allocate():
//   * um quadro nunca e dividido no fim da arena; se nao cabe, da a volta;
//   * os descritores estao sempre em ordem de captura;
//   * nenhum descritor aponta para bytes que ja foram sobrescritos.
class RingBuffer {
public:
    explicit RingBuffer(BufferCapacity capacity);

    // --- escrita ------------------------------------------------------------

    // Reserva espaco para um quadro e devolve o descritor a ser escrito.
    Allocation allocate(
        Nanoseconds captureTsNs,
        std::size_t length,
        bool isKeyframe,
        const std::string& sessionId
    );

    // --- leitura ------------------------------------------------------------

    // Descritores decodificaveis que cobrem a janela, com flags de truncamento.
    [[nodiscard]] BufferSegment segmentFor(const CaptureWindow& window) const;

    // --- inspecao -----------------------------------------------------------

    [[nodiscard]] bool empty() const noexcept { return m_frames.empty(); }
    [[nodiscard]] Nanoseconds newestCaptureTsNs() const;
    [[nodiscard]] Nanoseconds oldestCaptureTsNs() const;
    [[nodiscard]] const std::deque<FrameDescriptor>& frames() const noexcept { return m_frames; }
    [[nodiscard]] BufferStats stats() const;

private:
    void rejectIfTooLarge(std::size_t length) const;
    bool switchSessionIfNeeded(const std::string& sessionId);
    void rejectIfClockWentBackwards(Nanoseconds captureTsNs) const;
    std::size_t offsetFor(std::size_t length);
    void discardFramesBeyondWritePointer();
    std::vector<FrameDescriptor> evictOverlapping(std::size_t offset, std::size_t length);
    FrameDescriptor registerFrame(
        Nanoseconds captureTsNs,
        std::size_t offset,
        std::size_t length,
        bool isKeyframe,
        const std::string& sessionId
    );

    static void rejectIfWindowAbsent(
        const std::deque<FrameDescriptor>& frames,
        const CaptureWindow& window
    );
    static std::size_t decodableStartIndex(
        const std::deque<FrameDescriptor>& frames,
        const CaptureWindow& window
    );
    static std::size_t lastIndexWithin(
        const std::deque<FrameDescriptor>& frames,
        const CaptureWindow& window
    );

    std::size_t m_capacity;
    std::deque<FrameDescriptor> m_frames;
    std::size_t m_writeOffset{0};
    std::uint64_t m_nextSequence{0};
    bool m_hasSession{false};
    std::string m_sessionId;
    std::uint64_t m_ingestedTotal{0};
    std::uint64_t m_evictedTotal{0};
};

} // namespace ods::s4::domain
