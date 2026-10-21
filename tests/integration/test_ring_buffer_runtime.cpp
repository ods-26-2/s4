// S4.1 — testes de integracao: /dev/shm real, threads, fachada e cenario
// gravado. Inclui a prova de que a fachada real substitui o mock sem que o
// ExtractClipUseCase (S4.2) perceba a troca.
#include "tests/ods_check.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/domain/value_objects/time_window.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/ringbuffer/frame_log.hpp"
#include "s4/infrastructure/ringbuffer/shared_memory_frame_store.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "tests/unit/fakes/in_memory_file_storage.hpp"

using namespace ods::s4;
using domain::CaptureWindow;
using domain::kNanosecondsPerSecond;
using domain::Nanoseconds;

namespace {

constexpr Nanoseconds S = kNanosecondsPerSecond;
constexpr int kFps = 10;
constexpr int kGop = 10;

std::string unique_name(const std::string& prefix) {
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return prefix + std::to_string(++counter) + "_" + std::to_string(now % 100000);
}

// Um segundo de video sintetico = 1 keyframe de 6 kB + 9 deltas de 1,2 kB
// = 16,8 kB/s, ou seja ~134 kbps.
infrastructure::SyntheticSourceOptions synthetic_options(std::size_t maxFrames) {
    infrastructure::SyntheticSourceOptions options;
    options.fps = kFps;
    options.gop = kGop;
    options.keyframeBytes = 6000;
    options.deltaBytes = 1200;
    options.realtime = false;
    options.maxFrames = maxFrames;
    return options;
}

application::RingBufferConfig config(double windowSeconds) {
    application::RingBufferConfig config;
    config.cameraId = unique_name("test_cam_");
    config.windowSeconds = windowSeconds;
    config.bitrateBps = 134400;
    config.safetyFactor = 1.2;
    return config;
}

// Fonte controlada pelo teste: entrega quadros so quando mandado.
class ManualFrameSource : public domain::IFrameSource {
public:
    void start(domain::FrameCallback onFrame) override {
        m_onFrame = std::move(onFrame);
        started = true;
    }
    void stop() override { stopped = true; }
    [[nodiscard]] bool isRunning() const override { return started && !stopped; }

    void emit(Nanoseconds captureTsNs, bool isKeyframe = true, const std::string& session = "live") {
        static const std::vector<std::uint8_t> payload{0x42};
        domain::CapturedFrame frame;
        frame.captureTsNs = captureTsNs;
        frame.data = payload.data();
        frame.length = payload.size();
        frame.isKeyframe = isKeyframe;
        frame.sessionId = session;
        m_onFrame(frame);
    }

    bool started{false};
    bool stopped{false};

private:
    domain::FrameCallback m_onFrame;
};

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

std::unique_ptr<infrastructure::RingBufferRAMFacade> make_facade(
    application::RingBufferConfig cfg,
    std::unique_ptr<domain::IFrameSource> source
) {
    return std::make_unique<infrastructure::RingBufferRAMFacade>(cfg, std::move(source));
}

} // namespace

// ======================================================= SharedMemoryFrameStore

void test_deve_ler_de_volta_os_bytes_quando_escrever_em_um_offset() {
    infrastructure::SharedMemoryFrameStore store(unique_name("ods_t_"), 64);
    const std::vector<std::uint8_t> payload{'h', 'e', 'l', 'l', 'o'};

    store.write(10, payload.data(), payload.size());

    ODS_CHECK(store.read(10, 5) == payload);
}

void test_deve_ser_visivel_por_outro_mapeamento_quando_usar_o_mesmo_nome() {
    const std::string name = unique_name("ods_t_");
    infrastructure::SharedMemoryFrameStore owner(name, 64, true);
    const std::vector<std::uint8_t> payload{'s', 'h', 'a', 'r', 'e', 'd'};
    owner.write(0, payload.data(), payload.size());

    infrastructure::SharedMemoryFrameStore reader(name, 64, false);

    ODS_CHECK(reader.read(0, 6) == payload);
}

void test_deve_lancar_excecao_quando_acesso_sair_da_arena() {
    infrastructure::SharedMemoryFrameStore store(unique_name("ods_t_"), 16);
    const std::vector<std::uint8_t> payload(6, 'x');

    ODS_CHECK(throws<domain::FrameStoreError>(
        [&] { store.write(12, payload.data(), payload.size()); }));
    ODS_CHECK(throws<domain::FrameStoreError>([&] { store.read(12, 6); }));
}

void test_deve_lancar_excecao_quando_capacidade_do_segmento_for_zero() {
    ODS_CHECK(throws<domain::InvalidCapacityError>(
        [&] { infrastructure::SharedMemoryFrameStore(unique_name("ods_t_"), 0); }));
}

void test_deve_lancar_excecao_quando_abrir_segmento_inexistente() {
    ODS_CHECK(throws<domain::FrameStoreError>(
        [&] { infrastructure::SharedMemoryFrameStore(unique_name("ods_missing_"), 16, false); }));
}

void test_deve_lancar_excecao_quando_usar_arena_ja_fechada() {
    infrastructure::SharedMemoryFrameStore store(unique_name("ods_t_"), 16);
    store.close();
    ODS_CHECK(throws<domain::FrameStoreError>([&] { store.read(0, 4); }));
}

void test_deve_lancar_excecao_quando_segmento_ja_existir_e_substituicao_estiver_desligada() {
    const std::string name = unique_name("ods_t_");
    infrastructure::SharedMemoryFrameStore first(name, 64, true, false);

    ODS_CHECK(throws<domain::FrameStoreError>(
        [&] { infrastructure::SharedMemoryFrameStore(name, 64, true, false); }));
}

// Cenario real: o processo anterior morreu sem liberar /dev/shm. Sem
// replaceExisting o servico nao consegue subir de novo.
void test_deve_substituir_segmento_orfao_quando_replace_existing_estiver_ligado() {
    const std::string name = unique_name("ods_t_");
    {
        // Simula o orfao: cria o segmento e o "abandona" sem remover.
        infrastructure::SharedMemoryFrameStore stale(name, 64, true, false);
        const std::vector<std::uint8_t> payload{'o', 'l', 'd'};
        stale.write(0, payload.data(), payload.size());
    }
    infrastructure::SharedMemoryFrameStore leftover(name, 64, true, false);
    (void)leftover.capacityBytes();

    infrastructure::SharedMemoryFrameStore fresh(name, 64, true, true);

    ODS_CHECK(fresh.capacityBytes() == 64u);
}

// ========================================================== RingBufferRAMFacade

void test_deve_lancar_excecao_quando_extrair_antes_de_iniciar() {
    auto facade = make_facade(config(5.0), std::make_unique<ManualFrameSource>());
    ODS_CHECK(throws<domain::BufferNotRunningError>(
        [&] { (void)facade->extractAround(0, 1.0, 1.0); }));
}

void test_deve_iniciar_e_parar_a_fonte_quando_ciclo_de_vida_for_executado() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(5.0), std::move(source));

    facade->startCapture("ignored");
    ODS_CHECK(raw->started);
    facade->stopCapture();

    ODS_CHECK(raw->stopped);
    ODS_CHECK(!facade->isCapturing());
}

void test_deve_devolver_o_antes_do_evento_quando_o_depois_ja_estiver_no_buffer() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("ignored");
    for (int second = 0; second < 20; ++second) {
        raw->emit(second * S);
    }

    const auto segment = facade->extractAround(10 * S, 3.0, 2.0);

    ODS_CHECK(segment.firstCaptureTsNs() == 7 * S);
    ODS_CHECK(segment.lastCaptureTsNs() == 12 * S);
    ODS_CHECK(!segment.isTruncatedAtStart && !segment.isTruncatedAtEnd);
    facade->stopCapture();
}

void test_deve_esperar_o_depois_do_evento_quando_ainda_estiver_sendo_capturado() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("ignored");
    for (int second = 0; second <= 10; ++second) {
        raw->emit(second * S);
    }

    std::thread captureMore([raw] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (int second = 11; second <= 13; ++second) {
            raw->emit(second * S);
        }
    });
    const auto segment = facade->extractAround(10 * S, 2.0, 3.0, 2.0);
    captureMore.join();

    ODS_CHECK(segment.lastCaptureTsNs() == 13 * S);
    ODS_CHECK(!segment.isTruncatedAtEnd);
    facade->stopCapture();
}

void test_deve_devolver_trecho_truncado_quando_timeout_expirar_antes_do_depois() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("ignored");
    for (int second = 0; second <= 10; ++second) {
        raw->emit(second * S);
    }

    const auto segment = facade->extractAround(10 * S, 2.0, 5.0, 0.05);

    ODS_CHECK(segment.isTruncatedAtEnd);
    ODS_CHECK(segment.lastCaptureTsNs() == 10 * S);
    facade->stopCapture();
}

// Quem espera pelo pos-evento precisa ser liberado assim que a captura para,
// e nao so quando o timeout (aqui 10 s) expirar.
void test_deve_liberar_extracao_pendente_quando_captura_for_parada() {
    for (int round = 0; round < 20; ++round) {
        auto source = std::make_unique<ManualFrameSource>();
        auto* raw = source.get();
        auto facade = make_facade(config(30.0), std::move(source));
        facade->startCapture("ignored");
        raw->emit(0);

        bool stoppedWhileWaiting = false;
        const auto begin = std::chrono::steady_clock::now();
        std::thread waiter([&] {
            stoppedWhileWaiting = throws<domain::BufferNotRunningError>(
                [&] { (void)facade->extractAround(0, 0.0, 60.0, 10.0); });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 3));
        facade->stopCapture();
        waiter.join();
        const auto elapsed = std::chrono::steady_clock::now() - begin;

        ODS_CHECK(stoppedWhileWaiting);
        ODS_CHECK(elapsed < std::chrono::seconds(2));
    }
}

void test_deve_contar_quadro_rejeitado_quando_timestamp_retroceder() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("ignored");
    raw->emit(5 * S);

    raw->emit(4 * S);

    ODS_CHECK(facade->framesRejectedTotal() == 1u);
    ODS_CHECK(facade->stats().framesStored == 1u);
    facade->stopCapture();
}

void test_deve_reiniciar_quando_sessao_de_replay_mudar() {
    auto source = std::make_unique<ManualFrameSource>();
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("ignored");
    raw->emit(100 * S, true, "replay-1");

    raw->emit(3 * S, true, "replay-2");

    const auto stats = facade->stats();
    ODS_CHECK(stats.sessionId == "replay-2");
    ODS_CHECK(stats.framesStored == 1u);
    facade->stopCapture();
}

// ============================================== Ponta a ponta com memoria real

void test_deve_entregar_o_antes_e_o_depois_do_evento_quando_stream_sintetico_encher_o_buffer() {
    auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
        synthetic_options(30 * kFps));  // 30 s de video
    auto* raw = source.get();
    auto facade = make_facade(config(5.0), std::move(source));  // buffer de 5 s
    facade->startCapture("synthetic");
    raw->waitUntilFinished();

    const auto segment = facade->extractAround(27 * S, 2.0, 1.0);

    ODS_CHECK(segment.frames.front().isKeyframe);
    ODS_CHECK(segment.firstCaptureTsNs() == 25 * S);
    ODS_CHECK(segment.lastCaptureTsNs() == 28 * S);
    ODS_CHECK(!segment.isTruncatedAtStart && !segment.isTruncatedAtEnd);
    // Byte a byte: o que saiu do buffer e exatamente o que a fonte produziu.
    for (const auto& frame : segment.frames) {
        ODS_CHECK(frame.data == raw->payloadAt(static_cast<std::size_t>(frame.sequence)));
    }
    facade->stopCapture();
}

void test_deve_manter_apenas_a_janela_configurada_quando_stream_for_mais_longo_que_o_buffer() {
    auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
        synthetic_options(30 * kFps));
    auto* raw = source.get();
    auto facade = make_facade(config(5.0), std::move(source));
    facade->startCapture("synthetic");
    raw->waitUntilFinished();

    const auto stats = facade->stats();

    ODS_CHECK(stats.spanSeconds() >= 4.0 && stats.spanSeconds() <= 6.5);
    ODS_CHECK(stats.framesEvictedTotal > 0);
    ODS_CHECK(stats.fillRatio() <= 1.0);
    facade->stopCapture();
}

void test_deve_reproduzir_o_mesmo_resultado_quando_cenario_gravado_for_reexecutado() {
    const std::string scenario = "cenario_evento_porta";
    {
        infrastructure::FrameLogRecorder recorder(scenario);
        infrastructure::SyntheticFrameSource producer(synthetic_options(8 * kFps));
        producer.start([&recorder](const domain::CapturedFrame& frame) { recorder.record(frame); });
        producer.waitUntilFinished();
        recorder.close();
    }

    std::vector<std::vector<std::uint8_t>> results;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto source = std::make_unique<infrastructure::RecordedFrameSource>(scenario, "replay-1");
        auto* raw = source.get();
        auto facade = make_facade(config(10.0), std::move(source));
        facade->startCapture("recorded");
        raw->waitUntilFinished();
        const auto segment = facade->extractAround(5 * S, 1.5, 1.0);
        ODS_CHECK(segment.sessionId == "replay-1");
        results.push_back(segment.asElementaryStream());
        facade->stopCapture();
    }

    ODS_CHECK(results[0] == results[1]);
    ODS_CHECK(!results[0].empty());
    std::remove((scenario + ".idx").c_str());
    std::remove((scenario + ".bin").c_str());
}

// Indice apontando para fora do blob: o erro precisa aparecer na construcao,
// e nao como leitura fora da memoria dentro da thread de replay.
void test_deve_lancar_excecao_quando_indice_do_cenario_gravado_estiver_corrompido() {
    const std::string scenario = unique_name("cenario_corrompido_");
    {
        std::ofstream blob(scenario + ".bin", std::ios::binary);
        blob << "abcd";
        std::ofstream index(scenario + ".idx");
        index << "0 2 100 1 sessao\n";
    }

    ODS_CHECK(throws<domain::FrameSourceError>(
        [&] { infrastructure::RecordedFrameSource source(scenario); }));

    std::remove((scenario + ".idx").c_str());
    std::remove((scenario + ".bin").c_str());
}

void test_deve_lancar_excecao_quando_indice_do_cenario_gravado_nao_existir() {
    const std::string scenario = unique_name("cenario_sem_indice_");
    {
        std::ofstream blob(scenario + ".bin", std::ios::binary);
        blob << "abcd";
    }

    ODS_CHECK(throws<domain::FrameSourceError>(
        [&] { infrastructure::RecordedFrameSource source(scenario); }));

    std::remove((scenario + ".bin").c_str());
}

// ========================= Integracao com o S4.2: a fachada substitui o mock

void test_deve_extrair_clipe_pelo_use_case_quando_buffer_real_substituir_o_mock() {
    auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
        synthetic_options(20 * kFps));
    auto* raw = source.get();
    auto facade = std::make_shared<infrastructure::RingBufferRAMFacade>(
        config(30.0), std::move(source));
    facade->startCapture("synthetic");
    raw->waitUntilFinished();

    // O use case do S4.2 nao sabe que o mock foi trocado: ele so conhece a
    // porta IMediaBufferReader e o relogio de parede.
    auto repository = std::make_shared<infrastructure::InMemoryMediaClipRepository>();
    auto storage = std::make_shared<test::InMemoryFileStorage>();
    application::ExtractClipUseCase useCase(repository, facade, storage);

    const auto anchor = facade->stats();
    ODS_CHECK(anchor.hasFrames);
    const auto end = std::chrono::system_clock::now();
    const auto start = end - std::chrono::seconds(2);

    const auto clip = useCase.execute("evt-ring-buffer", domain::TimeWindow(start, end),
                                      "/tmp/evt-ring-buffer.mp4");

    ODS_CHECK(clip.eventId() == "evt-ring-buffer");
    ODS_CHECK(!clip.sha256Hash().empty());
    ODS_CHECK(clip.isRetained());
    facade->stopCapture();
}

void test_deve_devolver_lista_vazia_quando_janela_pedida_nao_existir_no_buffer() {
    auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
        synthetic_options(5 * kFps));
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("synthetic");
    raw->waitUntilFinished();

    // Janela muito no futuro: o contrato com o S4.2 e lista vazia, e nao
    // excecao — quem decide lancar MediaBufferEmptyError e o ExtractClipUseCase.
    const auto future = std::chrono::system_clock::now() + std::chrono::hours(1);
    const auto payloads = facade->readWindow(future, future + std::chrono::seconds(1));

    ODS_CHECK(payloads.empty());
    facade->stopCapture();
}

// Depois de stopCapture() a arena /dev/shm ja foi desmapeada, mas o S4.2 (via
// POST /api/v1/events) ainda pode chamar readWindow() durante o desligamento:
// o contrato continua sendo lista vazia, nunca FrameStoreError.
void test_deve_devolver_lista_vazia_quando_ler_janela_apos_parar_a_captura() {
    auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
        synthetic_options(5 * kFps));
    auto* raw = source.get();
    auto facade = make_facade(config(30.0), std::move(source));
    facade->startCapture("synthetic");
    raw->waitUntilFinished();
    const auto end = std::chrono::system_clock::now();
    const auto start = end - std::chrono::seconds(10);
    ODS_CHECK(!facade->readWindow(start, end).empty());

    facade->stopCapture();

    ODS_CHECK(facade->readWindow(start, end).empty());
    ODS_CHECK(facade->extractWindow(start, end).empty());
}

// Leituras concorrentes com o desligamento nao podem tocar a arena depois do
// munmap (isso seria acesso a memoria liberada, e nao uma excecao).
void test_deve_parar_sem_falha_quando_leituras_concorrerem_com_o_desligamento() {
    for (int round = 0; round < 20; ++round) {
        auto source = std::make_unique<infrastructure::SyntheticFrameSource>(
            synthetic_options(5 * kFps));
        auto* raw = source.get();
        auto facade = std::make_shared<infrastructure::RingBufferRAMFacade>(
            config(30.0), std::move(source));
        facade->startCapture("synthetic");
        raw->waitUntilFinished();
        const auto end = std::chrono::system_clock::now();
        const auto start = end - std::chrono::seconds(10);

        std::thread reader([facade, start, end] {
            for (int read = 0; read < 200; ++read) {
                (void)facade->readWindow(start, end);
            }
        });
        facade->stopCapture();
        reader.join();
        ODS_CHECK(facade->readWindow(start, end).empty());
    }
}

void test_deve_expor_pipelines_gstreamer_prontos_quando_consultados() {
    infrastructure::CsiCameraOptions camera;
    const std::string csi = infrastructure::GStreamerAppsinkFrameSource::jetsonCsiH264Pipeline(camera);
    ODS_CHECK(csi.find("nvarguscamerasrc") != std::string::npos);
    ODS_CHECK(csi.find("insert-sps-pps=true") != std::string::npos);
    ODS_CHECK(csi.find("appsink name=ods_sink") != std::string::npos);

    const std::string shm = infrastructure::GStreamerAppsinkFrameSource::shmH264Pipeline("/dev/shm/p4");
    // O caminho vai entre aspas: sem isso um socket path com espaco (ou o
    // nome do arquivo mp4 no replay) truncaria o parse do pipeline.
    ODS_CHECK(shm.find("shmsrc socket-path=\"/dev/shm/p4\"") != std::string::npos);
}

int main() {
    test_deve_ler_de_volta_os_bytes_quando_escrever_em_um_offset();
    test_deve_ser_visivel_por_outro_mapeamento_quando_usar_o_mesmo_nome();
    test_deve_lancar_excecao_quando_acesso_sair_da_arena();
    test_deve_lancar_excecao_quando_capacidade_do_segmento_for_zero();
    test_deve_lancar_excecao_quando_abrir_segmento_inexistente();
    test_deve_lancar_excecao_quando_usar_arena_ja_fechada();
    test_deve_lancar_excecao_quando_segmento_ja_existir_e_substituicao_estiver_desligada();
    test_deve_substituir_segmento_orfao_quando_replace_existing_estiver_ligado();

    test_deve_lancar_excecao_quando_extrair_antes_de_iniciar();
    test_deve_iniciar_e_parar_a_fonte_quando_ciclo_de_vida_for_executado();
    test_deve_devolver_o_antes_do_evento_quando_o_depois_ja_estiver_no_buffer();
    test_deve_esperar_o_depois_do_evento_quando_ainda_estiver_sendo_capturado();
    test_deve_devolver_trecho_truncado_quando_timeout_expirar_antes_do_depois();
    test_deve_liberar_extracao_pendente_quando_captura_for_parada();
    test_deve_contar_quadro_rejeitado_quando_timestamp_retroceder();
    test_deve_reiniciar_quando_sessao_de_replay_mudar();

    test_deve_entregar_o_antes_e_o_depois_do_evento_quando_stream_sintetico_encher_o_buffer();
    test_deve_manter_apenas_a_janela_configurada_quando_stream_for_mais_longo_que_o_buffer();
    test_deve_reproduzir_o_mesmo_resultado_quando_cenario_gravado_for_reexecutado();
    test_deve_lancar_excecao_quando_indice_do_cenario_gravado_estiver_corrompido();
    test_deve_lancar_excecao_quando_indice_do_cenario_gravado_nao_existir();

    test_deve_extrair_clipe_pelo_use_case_quando_buffer_real_substituir_o_mock();
    test_deve_devolver_lista_vazia_quando_janela_pedida_nao_existir_no_buffer();
    test_deve_devolver_lista_vazia_quando_ler_janela_apos_parar_a_captura();
    test_deve_parar_sem_falha_quando_leituras_concorrerem_com_o_desligamento();
    test_deve_expor_pipelines_gstreamer_prontos_quando_consultados();

    std::cout << "test_s4_ring_buffer_runtime: all tests passed\n";
    return 0;
}
