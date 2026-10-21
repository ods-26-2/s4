#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "s4/domain/repositories/frame_store.hpp"

namespace ods::s4::infrastructure {

// IFrameStore sobre memoria compartilhada POSIX.
//
// shm_open + ftruncate + mmap criam o segmento em /dev/shm/<nome> — um tmpfs
// que vive 100% em RAM. Tres motivos para usar isso em vez de um vetor comum:
//   1. zero desgaste do NVMe (gravar 500 kB/s em flash 24 h por dia consome
//      ciclos de escrita a toa);
//   2. o segmento e VISIVEL a outros processos: se o S4.2 rodar em processo
//      separado, ele mapeia a mesma regiao e le os bytes pelo offset do
//      descritor, sem copia extra;
//   3. e o mesmo mecanismo que o shmsink/shmsrc do GStreamer (P4) ja usa.
class SharedMemoryFrameStore : public domain::IFrameStore {
public:
    // create=true cria (e no fim remove) o segmento; create=false apenas mapeia
    // um segmento existente, que e como um processo leitor o enxerga.
    //
    // replaceExisting trata o segmento ORFAO: se um processo anterior morreu
    // sem liberar /dev/shm/<nome>, o arquivo continua la e a criacao exclusiva
    // falha, impedindo o servico de subir de novo. Com replaceExisting=true o
    // segmento antigo e removido antes da criacao. Fica em false por padrao
    // para que duas instancias da MESMA camera nao se sobrescrevam em silencio.
    SharedMemoryFrameStore(
        const std::string& name,
        std::size_t capacityBytes,
        bool create = true,
        bool replaceExisting = false
    );
    ~SharedMemoryFrameStore() override;

    SharedMemoryFrameStore(const SharedMemoryFrameStore&) = delete;
    SharedMemoryFrameStore& operator=(const SharedMemoryFrameStore&) = delete;

    [[nodiscard]] std::size_t capacityBytes() const override { return m_capacity; }
    void write(std::size_t offset, const std::uint8_t* data, std::size_t length) override;
    [[nodiscard]] std::vector<std::uint8_t> read(std::size_t offset, std::size_t length) const override;
    void close() override;

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] std::string path() const;

private:
    void rejectOutOfBounds(std::size_t start, std::size_t end) const;

    std::string m_name;
    std::size_t m_capacity;
    bool m_ownsSegment;
    bool m_isClosed{false};
    std::uint8_t* m_arena{nullptr};
    int m_fd{-1};
};

} // namespace ods::s4::infrastructure
