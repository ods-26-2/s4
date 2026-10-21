// S4.1 — testes de unidade do ring buffer: dominio puro, sem threads e sem I/O.
#include "tests/ods_check.hpp"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/application/use_cases/extract_capture_window.hpp"
#include "s4/application/use_cases/ingest_frame.hpp"
#include "s4/domain/entities/ring_buffer.hpp"
#include "s4/domain/value_objects/buffer_capacity.hpp"
#include "s4/domain/value_objects/capture_window.hpp"

using namespace ods::s4;
using domain::CaptureWindow;
using domain::kNanosecondsPerSecond;
using domain::Nanoseconds;

namespace {

constexpr Nanoseconds S = kNanosecondsPerSecond;
const std::string kSession = "sessao-1";

// IFrameStore em memoria: mesma semantica da arena real, sem sistema
// operacional no caminho.
class InMemoryFrameStore : public domain::IFrameStore {
public:
    explicit InMemoryFrameStore(std::size_t capacityBytes) : m_arena(capacityBytes, 0) {}

    [[nodiscard]] std::size_t capacityBytes() const override { return m_arena.size(); }

    void write(std::size_t offset, const std::uint8_t* data, std::size_t length) override {
        std::memcpy(m_arena.data() + offset, data, length);
    }

    [[nodiscard]] std::vector<std::uint8_t> read(std::size_t offset, std::size_t length) const override {
        return std::vector<std::uint8_t>(
            m_arena.begin() + static_cast<std::ptrdiff_t>(offset),
            m_arena.begin() + static_cast<std::ptrdiff_t>(offset + length)
        );
    }

    void close() override { isClosed = true; }

    bool isClosed{false};

private:
    std::vector<std::uint8_t> m_arena;
};

domain::RingBuffer make_buffer(std::size_t capacityBytes = 100) {
    return domain::RingBuffer(domain::BufferCapacity(capacityBytes));
}

domain::Allocation push(
    domain::RingBuffer& buffer,
    Nanoseconds tsNs,
    std::size_t length,
    bool keyframe = true,
    const std::string& session = kSession
) {
    return buffer.allocate(tsNs, length, keyframe, session);
}

// Quadros a cada 1/fps s; keyframe no inicio de cada segundo.
void build_gop_stream(domain::RingBuffer& buffer, int seconds, int fps = 10, int gop = 10) {
    for (int index = 0; index < seconds * fps; ++index) {
        push(buffer, index * S / fps, 1, index % gop == 0);
    }
}

domain::CapturedFrame make_frame(
    Nanoseconds tsNs,
    const std::vector<std::uint8_t>& payload,
    bool keyframe = true,
    const std::string& session = "s"
) {
    domain::CapturedFrame frame;
    frame.captureTsNs = tsNs;
    frame.data = payload.data();
    frame.length = payload.size();
    frame.isKeyframe = keyframe;
    frame.sessionId = session;
    return frame;
}

template <typename Exception, typename Callable>
bool throws(Callable&& callable) {
    try {
        callable();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

} // namespace

// ================================================================ CaptureWindow

void test_deve_calcular_duracao_corretamente_quando_janela_for_valida() {
    const CaptureWindow window(10 * S, 40 * S);
    ODS_CHECK(window.durationSeconds() >= 29.999 && window.durationSeconds() <= 30.001);
}

void test_deve_lancar_excecao_quando_start_for_maior_ou_igual_ao_end() {
    ODS_CHECK(throws<domain::InvalidTimeWindowError>([] { CaptureWindow(10 * S, 10 * S); }));
}

void test_deve_criar_janela_em_torno_do_evento_quando_pre_e_pos_forem_informados() {
    const CaptureWindow window = CaptureWindow::around(100 * S, 15.0, 5.0);
    ODS_CHECK(window.startNs() == 85 * S);
    ODS_CHECK(window.endNs() == 105 * S);
}

void test_deve_lancar_excecao_quando_pre_ou_pos_forem_negativos() {
    ODS_CHECK(throws<domain::InvalidTimeWindowError>([] { (void)CaptureWindow::around(100 * S, -1.0, 5.0); }));
}

void test_deve_conter_extremos_quando_timestamp_estiver_na_borda() {
    const CaptureWindow window(5, 9);
    ODS_CHECK(window.contains(5));
    ODS_CHECK(window.contains(9));
    ODS_CHECK(!window.contains(10));
}

void test_deve_detectar_sobreposicao_quando_janelas_se_tocarem() {
    const CaptureWindow first(0, 10);
    ODS_CHECK(first.overlaps(CaptureWindow(10, 20)));
    ODS_CHECK(!first.overlaps(CaptureWindow(11, 20)));
}

// =============================================================== BufferCapacity

void test_deve_calcular_bytes_quando_stream_for_codificado() {
    ODS_CHECK(domain::BufferCapacity::forEncodedStream(4000000, 30.0, 1.0).totalBytes() == 15000000u);
}

void test_deve_aplicar_folga_quando_safety_factor_for_informado() {
    ODS_CHECK(domain::BufferCapacity::forEncodedStream(4000000, 30.0, 1.5).totalBytes() == 22500000u);
}

void test_deve_calcular_bytes_quando_stream_for_raw() {
    ODS_CHECK(domain::BufferCapacity::forRawStream(1920, 1080, 1.5, 30.0, 1.0).totalBytes() == 93312000u);
}

void test_deve_lancar_excecao_quando_capacidade_for_zero() {
    ODS_CHECK(throws<domain::InvalidCapacityError>([] { domain::BufferCapacity(0); }));
}

void test_deve_derivar_nome_do_segmento_quando_config_nao_informar() {
    application::RingBufferConfig config;
    config.cameraId = "cam0";
    ODS_CHECK(config.resolvedShmName() == "ods_s4_ring_cam0");
}

// =========================================================== RingBuffer: escrita

void test_deve_alocar_offsets_sequenciais_quando_houver_espaco() {
    auto buffer = make_buffer(100);
    const auto first = push(buffer, 0, 30).descriptor;
    const auto second = push(buffer, 1, 30).descriptor;

    ODS_CHECK(first.offset == 0u && second.offset == 30u);
    ODS_CHECK(first.sequence == 0u && second.sequence == 1u);
}

void test_deve_voltar_ao_inicio_quando_quadro_nao_couber_no_fim_da_arena() {
    auto buffer = make_buffer(100);
    push(buffer, 0, 40);
    push(buffer, 1, 40);

    ODS_CHECK(push(buffer, 2, 40).descriptor.offset == 0u);
}

void test_deve_expulsar_o_mais_antigo_quando_sobrescrever_sua_regiao() {
    auto buffer = make_buffer(100);
    const auto oldest = push(buffer, 0, 40).descriptor;
    push(buffer, 1, 40);

    const auto allocation = push(buffer, 2, 40);

    ODS_CHECK(allocation.evicted.size() == 1u);
    ODS_CHECK(allocation.evicted.front().sequence == oldest.sequence);
    ODS_CHECK(buffer.frames().size() == 2u);
}

void test_deve_descartar_sobras_da_volta_anterior_quando_der_a_volta() {
    auto buffer = make_buffer(100);
    push(buffer, 0, 30);                                        // [0, 30)
    push(buffer, 1, 30);                                        // [30, 60)
    const std::uint64_t leftover = push(buffer, 2, 30).descriptor.sequence;  // [60, 90)
    push(buffer, 3, 50);  // nao cabe em [90,100) -> volta para [0,50)

    bool leftoverAlive = false;
    for (const auto& frame : buffer.frames()) {
        leftoverAlive = leftoverAlive || frame.sequence == leftover;
    }
    ODS_CHECK(leftoverAlive);

    push(buffer, 4, 60);  // nao cabe em [50,100) -> volta e descarta a sobra

    ODS_CHECK(buffer.frames().size() == 1u);
    ODS_CHECK(buffer.frames().front().captureTsNs == 4);
}

void test_deve_lancar_excecao_quando_quadro_for_maior_que_a_arena() {
    auto buffer = make_buffer(100);
    ODS_CHECK(throws<domain::FrameTooLargeError>([&] { push(buffer, 0, 101); }));
}

void test_deve_lancar_excecao_quando_quadro_tiver_tamanho_zero() {
    auto buffer = make_buffer(100);
    ODS_CHECK(throws<domain::FrameTooLargeError>([&] { push(buffer, 0, 0); }));
}

void test_deve_lancar_excecao_quando_timestamp_retroceder_na_mesma_sessao() {
    auto buffer = make_buffer(100);
    push(buffer, 10, 10);
    ODS_CHECK(throws<domain::NonMonotonicTimestampError>([&] { push(buffer, 9, 10); }));
}

void test_deve_aceitar_timestamp_igual_ao_anterior_quando_dois_quadros_compartilharem_instante() {
    auto buffer = make_buffer(100);
    push(buffer, 10, 10);
    push(buffer, 10, 10);
    ODS_CHECK(buffer.frames().size() == 2u);
}

void test_deve_reiniciar_o_buffer_quando_a_sessao_mudar() {
    auto buffer = make_buffer(100);
    push(buffer, 50, 30, true, "replay-a");

    const auto allocation = push(buffer, 5, 30, true, "replay-b");

    ODS_CHECK(allocation.sessionRestarted);
    ODS_CHECK(allocation.descriptor.offset == 0u);
    ODS_CHECK(buffer.frames().size() == 1u);
    ODS_CHECK(buffer.frames().front().sessionId == "replay-b");
}

// O teste mais importante do modulo: prova que nenhum clipe pode sair com
// quadros corrompidos por reutilizacao de memoria.
void test_nunca_deve_manter_descritor_apontando_para_bytes_sobrescritos() {
    constexpr std::size_t kCapacity = 1000;
    auto buffer = make_buffer(kCapacity);
    std::vector<std::uint8_t> arena(kCapacity, 0);
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> written;
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> sizeOf(1, 150);

    for (int step = 0; step < 2000; ++step) {
        const std::size_t length = sizeOf(rng);
        const auto descriptor = push(buffer, step, length).descriptor;
        const std::vector<std::uint8_t> payload(length, static_cast<std::uint8_t>(step % 256));
        std::copy(payload.begin(), payload.end(),
                  arena.begin() + static_cast<std::ptrdiff_t>(descriptor.offset));
        written[descriptor.sequence] = payload;

        for (const auto& live : buffer.frames()) {
            const std::vector<std::uint8_t> actual(
                arena.begin() + static_cast<std::ptrdiff_t>(live.offset),
                arena.begin() + static_cast<std::ptrdiff_t>(live.endOffset())
            );
            ODS_CHECK(actual == written[live.sequence]);
        }
    }
}

// =========================================================== RingBuffer: leitura

void test_deve_comecar_no_ultimo_keyframe_anterior_quando_inicio_cair_no_meio_do_gop() {
    auto buffer = make_buffer(10000);
    build_gop_stream(buffer, 10);

    const auto segment = buffer.segmentFor(CaptureWindow(35 * S / 10, 5 * S));

    ODS_CHECK(segment.frames.front().isKeyframe);
    ODS_CHECK(segment.firstCaptureTsNs() == 3 * S);
    ODS_CHECK(segment.lastCaptureTsNs() == 5 * S);
    ODS_CHECK(!segment.isTruncatedAtStart && !segment.isTruncatedAtEnd);
}

void test_deve_marcar_truncamento_no_inicio_quando_janela_pedir_antes_do_quadro_mais_antigo() {
    auto buffer = make_buffer(10000);
    build_gop_stream(buffer, 10);

    const auto segment = buffer.segmentFor(CaptureWindow(-5 * S, 2 * S));

    ODS_CHECK(segment.isTruncatedAtStart);
    ODS_CHECK(segment.firstCaptureTsNs() == 0);
}

void test_deve_marcar_truncamento_no_fim_quando_o_depois_ainda_nao_foi_capturado() {
    auto buffer = make_buffer(10000);
    build_gop_stream(buffer, 10);

    const auto segment = buffer.segmentFor(CaptureWindow(8 * S, 20 * S));

    ODS_CHECK(segment.isTruncatedAtEnd);
    ODS_CHECK(segment.lastCaptureTsNs() == 99 * S / 10);
}

void test_deve_lancar_excecao_quando_nenhum_quadro_estiver_na_janela() {
    auto buffer = make_buffer(10000);
    build_gop_stream(buffer, 10);
    ODS_CHECK(throws<domain::WindowNotInBufferError>(
        [&] { (void)buffer.segmentFor(CaptureWindow(50 * S, 60 * S)); }));
}

void test_deve_lancar_excecao_quando_buffer_estiver_vazio() {
    auto buffer = make_buffer(10000);
    ODS_CHECK(throws<domain::WindowNotInBufferError>(
        [&] { (void)buffer.segmentFor(CaptureWindow(0, S)); }));
}

void test_deve_usar_primeiro_keyframe_dentro_da_janela_quando_o_anterior_ja_foi_expulso() {
    auto buffer = make_buffer(10000);
    push(buffer, 0, 1, false);  // P-frame orfao (keyframe ja expulso)
    push(buffer, 1 * S, 1, false);
    push(buffer, 2 * S, 1, true);
    push(buffer, 3 * S, 1, false);

    const auto segment = buffer.segmentFor(CaptureWindow(5 * S / 10, 3 * S));

    ODS_CHECK(segment.firstCaptureTsNs() == 2 * S);
}

// Pediu-se a partir de 0,5 s e o trecho so comeca em 2 s: 1,5 s do "antes"
// ficou de fora, ainda que o buffer tenha quadros (P orfaos) desde 0 s.
void test_deve_marcar_truncamento_no_inicio_quando_keyframe_anterior_ja_foi_expulso() {
    auto buffer = make_buffer(10000);
    push(buffer, 0, 1, false);
    push(buffer, 1 * S, 1, false);
    push(buffer, 2 * S, 1, true);
    push(buffer, 3 * S, 1, false);

    const auto segment = buffer.segmentFor(CaptureWindow(5 * S / 10, 3 * S));

    ODS_CHECK(segment.isTruncatedAtStart);
}

void test_nao_deve_marcar_truncamento_no_inicio_quando_trecho_recuar_ate_keyframe_anterior() {
    auto buffer = make_buffer(10000);
    build_gop_stream(buffer, 10);

    const auto segment = buffer.segmentFor(CaptureWindow(35 * S / 10, 5 * S));

    ODS_CHECK(segment.firstCaptureTsNs() < 35 * S / 10);
    ODS_CHECK(!segment.isTruncatedAtStart);
}

void test_deve_lancar_excecao_quando_nao_houver_keyframe_decodificavel() {
    auto buffer = make_buffer(10000);
    push(buffer, 0, 1, false);
    push(buffer, S, 1, false);
    ODS_CHECK(throws<domain::NoDecodableStartError>(
        [&] { (void)buffer.segmentFor(CaptureWindow(0, S)); }));
}

// ====================================================== RingBuffer: estatisticas

void test_deve_reportar_estatisticas_coerentes_quando_houver_expulsoes() {
    auto buffer = make_buffer(100);
    push(buffer, 0, 40);
    push(buffer, 1 * S, 40);
    push(buffer, 2 * S, 40);

    const auto stats = buffer.stats();

    ODS_CHECK(stats.framesIngestedTotal == 3u);
    ODS_CHECK(stats.framesEvictedTotal == 1u);
    ODS_CHECK(stats.framesStored == 2u);
    ODS_CHECK(stats.bytesUsed == 80u);
    ODS_CHECK(stats.spanSeconds() >= 0.999 && stats.spanSeconds() <= 1.001);
    ODS_CHECK(stats.fillRatio() >= 0.799 && stats.fillRatio() <= 0.801);
    ODS_CHECK(stats.sessionId == kSession);
}

void test_deve_reportar_span_zero_quando_buffer_estiver_vazio() {
    auto buffer = make_buffer();
    ODS_CHECK(buffer.stats().spanSeconds() == 0.0);
}

// ================================================================= Casos de uso

void test_deve_gravar_bytes_no_offset_alocado_quando_ingerir_quadro() {
    auto ring = make_buffer(64);
    InMemoryFrameStore store(64);
    application::IngestFrameUseCase ingest(ring, store);
    application::ExtractCaptureWindowUseCase extract(ring, store);
    const std::vector<std::uint8_t> first{'A', 'A', 'A', 'A'};
    const std::vector<std::uint8_t> second{'B', 'B', 'B', 'B', 'B', 'B'};

    ingest.execute(make_frame(0, first));
    ingest.execute(make_frame(S, second));
    const auto segment = extract.execute(CaptureWindow(0, S));

    ODS_CHECK(segment.frames.size() == 2u);
    ODS_CHECK(segment.frames[0].data == first);
    ODS_CHECK(segment.frames[1].data == second);
    ODS_CHECK(segment.sessionId == "s");
    ODS_CHECK(segment.asElementaryStream().size() == 10u);
}

void test_deve_copiar_os_bytes_quando_extrair_para_que_sobrescrita_posterior_nao_altere_o_trecho() {
    auto ring = make_buffer(64);
    InMemoryFrameStore store(64);
    application::IngestFrameUseCase ingest(ring, store);
    application::ExtractCaptureWindowUseCase extract(ring, store);
    const std::vector<std::uint8_t> original(60, 'X');
    ingest.execute(make_frame(0, original));
    const auto segment = extract.execute(CaptureWindow(0, S));

    ingest.execute(make_frame(S, std::vector<std::uint8_t>(60, 'Y')));  // sobrescreve tudo

    ODS_CHECK(segment.frames.front().data == original);
}

void test_deve_propagar_excecao_do_dominio_quando_janela_nao_existir() {
    auto ring = make_buffer(64);
    InMemoryFrameStore store(64);
    application::ExtractCaptureWindowUseCase extract(ring, store);
    ODS_CHECK(throws<domain::WindowNotInBufferError>([&] { (void)extract.execute(CaptureWindow(0, S)); }));
}

void test_deve_preservar_sequencia_e_flags_quando_materializar_quadros() {
    auto ring = make_buffer(64);
    InMemoryFrameStore store(64);
    application::IngestFrameUseCase ingest(ring, store);
    application::ExtractCaptureWindowUseCase extract(ring, store);
    ingest.execute(make_frame(0, std::vector<std::uint8_t>{'I'}, true));
    ingest.execute(make_frame(1, std::vector<std::uint8_t>{'P'}, false));

    const auto segment = extract.execute(CaptureWindow(0, 1));

    ODS_CHECK(segment.frames[0].sequence == 0u);
    ODS_CHECK(segment.frames[0].isKeyframe);
    ODS_CHECK(!segment.frames[1].isKeyframe);
    ODS_CHECK(segment.totalBytes() == 2u);
}

int main() {
    test_deve_calcular_duracao_corretamente_quando_janela_for_valida();
    test_deve_lancar_excecao_quando_start_for_maior_ou_igual_ao_end();
    test_deve_criar_janela_em_torno_do_evento_quando_pre_e_pos_forem_informados();
    test_deve_lancar_excecao_quando_pre_ou_pos_forem_negativos();
    test_deve_conter_extremos_quando_timestamp_estiver_na_borda();
    test_deve_detectar_sobreposicao_quando_janelas_se_tocarem();

    test_deve_calcular_bytes_quando_stream_for_codificado();
    test_deve_aplicar_folga_quando_safety_factor_for_informado();
    test_deve_calcular_bytes_quando_stream_for_raw();
    test_deve_lancar_excecao_quando_capacidade_for_zero();
    test_deve_derivar_nome_do_segmento_quando_config_nao_informar();

    test_deve_alocar_offsets_sequenciais_quando_houver_espaco();
    test_deve_voltar_ao_inicio_quando_quadro_nao_couber_no_fim_da_arena();
    test_deve_expulsar_o_mais_antigo_quando_sobrescrever_sua_regiao();
    test_deve_descartar_sobras_da_volta_anterior_quando_der_a_volta();
    test_deve_lancar_excecao_quando_quadro_for_maior_que_a_arena();
    test_deve_lancar_excecao_quando_quadro_tiver_tamanho_zero();
    test_deve_lancar_excecao_quando_timestamp_retroceder_na_mesma_sessao();
    test_deve_aceitar_timestamp_igual_ao_anterior_quando_dois_quadros_compartilharem_instante();
    test_deve_reiniciar_o_buffer_quando_a_sessao_mudar();
    test_nunca_deve_manter_descritor_apontando_para_bytes_sobrescritos();

    test_deve_comecar_no_ultimo_keyframe_anterior_quando_inicio_cair_no_meio_do_gop();
    test_deve_marcar_truncamento_no_inicio_quando_janela_pedir_antes_do_quadro_mais_antigo();
    test_deve_marcar_truncamento_no_fim_quando_o_depois_ainda_nao_foi_capturado();
    test_deve_lancar_excecao_quando_nenhum_quadro_estiver_na_janela();
    test_deve_lancar_excecao_quando_buffer_estiver_vazio();
    test_deve_usar_primeiro_keyframe_dentro_da_janela_quando_o_anterior_ja_foi_expulso();
    test_deve_marcar_truncamento_no_inicio_quando_keyframe_anterior_ja_foi_expulso();
    test_nao_deve_marcar_truncamento_no_inicio_quando_trecho_recuar_ate_keyframe_anterior();
    test_deve_lancar_excecao_quando_nao_houver_keyframe_decodificavel();

    test_deve_reportar_estatisticas_coerentes_quando_houver_expulsoes();
    test_deve_reportar_span_zero_quando_buffer_estiver_vazio();

    test_deve_gravar_bytes_no_offset_alocado_quando_ingerir_quadro();
    test_deve_copiar_os_bytes_quando_extrair_para_que_sobrescrita_posterior_nao_altere_o_trecho();
    test_deve_propagar_excecao_do_dominio_quando_janela_nao_existir();
    test_deve_preservar_sequencia_e_flags_quando_materializar_quadros();

    std::cout << "test_s4_ring_buffer: all tests passed\n";
    return 0;
}
