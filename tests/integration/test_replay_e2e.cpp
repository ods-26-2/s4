// S4 — ponta a ponta: cenario gravado -> ring buffer -> POST /api/v1/events ->
// GET /api/v1/clips/{id} -> SHA-256 do arquivo.
//
// Dois cenarios sao exercitados: o formato custom (.idx/.bin), que nao depende de
// camera nem de GStreamer, e um MP4 real de video_test/ (so com GStreamer). O
// cenario custom e GERADO pelo proprio teste, entao um "arquivo nao encontrado"
// nunca pode mais virar um falso verde.
#include "tests/ods_check.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"

#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/infrastructure/database/in_memory_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/gstreamer_mp4_muxer.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/hashing/sha256_hasher.hpp"
#include "s4/infrastructure/ringbuffer/frame_log.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#ifdef ODS_S4_WITH_GSTREAMER
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#endif

using namespace ods::s4;
using namespace std::chrono_literals;

namespace {

constexpr int kFps = 10;
constexpr int kGop = 10;  // 1 s entre keyframes

std::string sha256_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    ODS_CHECK(static_cast<bool>(file));
    const std::vector<std::uint8_t> data(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );
    return infrastructure::compute_sha256_hex(data);
}

// Extracao de campo de string do JSON. O servidor emite JSON gerado a mao e
// plano (sem espacos), o que torna isto seguro para o escopo do teste.
std::string json_string(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) return {};
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) return {};
    const std::size_t open = body.find('"', colon + 1);
    if (open == std::string::npos) return {};
    const std::size_t close = body.find('"', open + 1);
    if (close == std::string::npos) return {};
    return body.substr(open + 1, close - open - 1);
}

std::optional<long long> json_int_optional(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) return std::nullopt;
    const std::size_t colon = body.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) return std::nullopt;
    const std::size_t begin = body.find_first_not_of(" \t\r\n", colon + 1);
    if (begin == std::string::npos) return std::nullopt;
    std::size_t end = begin;
    while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) ||
                                 body[end] == '-' || body[end] == '+')) {
        ++end;
    }
    if (end == begin) return std::nullopt;
    try {
        return std::stoll(body.substr(begin, end - begin));
    } catch (...) {
        return std::nullopt;
    }
}

// Campos booleanos saem sem aspas no JSON, entao nao passam por json_string().
bool json_bool(const std::string& body, const std::string& key, bool expected) {
    const std::string pattern = "\"" + key + "\": ";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) return false;
    return body.compare(keyPos + pattern.size(), 4, expected ? "true" : "fals") == 0;
}

bool json_bool(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\": ";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) return false;
    return body.compare(keyPos + pattern.size(), 4, "true") == 0;
}

// capture_ts_ns e um int64 que nao cabe em int: estouraria em ~2^31 ns, isto
// e, 2 s de relogio de captura. Por isso o long long e obrigatorio aqui.
long long json_int(const std::string& body, const std::string& key) {
    const auto value = json_int_optional(body, key);
    ODS_CHECK(value.has_value());
    return value.value_or(0);
}

double json_double(const std::string& body, const std::string& key) {
    const std::string pattern = "\"" + key + "\": ";
    const std::size_t keyPos = body.find(pattern);
    if (keyPos == std::string::npos) return 0.0;
    return std::stod(body.substr(keyPos + pattern.size()));
}

// Sem ponto decimal: o JSON de um double precisa ser distinguivel de um int
// para o servidor parsear como double, e 1.0 != 1 na gramatica do JSON.
std::string format_double(double value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

// Procura um marcador binario no arquivo. Usado para provar que o container
// tem a assinatura de MP4, e nao apenas bytes com o nome certo.
bool file_contains(const std::string& path, const std::string& needle) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    const std::string content(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );
    return content.find(needle) != std::string::npos;
}

std::string unique_base(const std::string& prefix) {
    static int counter = 0;
    return prefix + std::to_string(++counter) + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
}

// Caminho do frame log do cenario gravado, sempre dentro do diretorio
// temporario.
//
// O caminho base nao e relativo. Um caminho relativo punha o
// .idx/.bin no diretorio de trabalho do ctest, que e a raiz do projeto: cada
// execucao escrevia lixo na arvore de codigo, e uma execucao que abortasse no
// meio deixava o par de arquivos para tras, a ponto de entrarem no commit sem
// ninguem ter visto. No diretorio temporario o pior caso e um /tmp sujo.
std::string scenario_base() {
    return (std::filesystem::temp_directory_path() /
            unique_base("s4_scenario_")).string();
}

// Grava o cenario custom com a MESMA forma de payload que o SyntheticFrameSource
// usa em test_ring_buffer_runtime.cpp, para que os dois testes fiquem compativeis.
std::string generate_scenario(const std::string& basePath, int seconds) {
    infrastructure::SyntheticSourceOptions options;
    options.fps = kFps;
    options.gop = kGop;
    options.keyframeBytes = 6000;
    options.deltaBytes = 1200;
    options.sessionId = "cenario-replay";
    options.realtime = false;
    options.maxFrames = seconds * kFps;

    infrastructure::FrameLogRecorder recorder(basePath);
    infrastructure::SyntheticFrameSource producer(options);
    producer.start([&recorder](const domain::CapturedFrame& frame) { recorder.record(frame); });
    producer.waitUntilFinished();
    recorder.close();
    return basePath;
}

struct ClipResult {
    int status{0};
    std::string body;
    std::string clipId;
    std::string fileUri;
    std::string sha256Hash;
    std::string startTime;
    std::string endTime;
};

struct BufferStatsView {
    bool has_frames{false};
    long long oldest_capture_ts_ns{0};
    long long newest_capture_ts_ns{0};
    double span_seconds{0.0};
    long long capacity_bytes{0};
    long long bytes_used{0};
    long long frames_stored{0};
};

class ReplayFixture {
public:
    explicit ReplayFixture(std::unique_ptr<domain::IFrameSource> source)
        : m_repository(std::make_shared<infrastructure::InMemoryMediaClipRepository>()),
          m_mediaDir(std::filesystem::temp_directory_path() / unique_base("s4_replay_media_")) {
        std::error_code ec;
        std::filesystem::create_directories(m_mediaDir, ec);
        ODS_CHECK(!ec);

        application::RingBufferConfig bufferConfig;
        bufferConfig.cameraId = unique_base("cam_replay_");
        bufferConfig.windowSeconds = 30.0;
        bufferConfig.bitrateBps = 134400;
        bufferConfig.safetyFactor = 1.2;
        bufferConfig.width = 320;
        bufferConfig.height = 240;
        bufferConfig.replaceStaleSegment = true;

        m_buffer = std::make_shared<infrastructure::RingBufferRAMFacade>(
            bufferConfig, std::move(source)
        );
        auto storage = std::make_shared<infrastructure::FileStorage>();
        // O caso de uso recebe as DUAS portas do fluxo real: leitura em
        // relogio de captura e muxing. Sem o muxer ele cairia no caminho
        // legado e gravaria Annex-B com extensao .mp4.
        m_useCase = std::make_shared<application::ExtractClipUseCase>(
            m_repository, m_buffer, storage, m_buffer,
            std::make_shared<infrastructure::GStreamerMp4Muxer>()
        );
        ODS_CHECK(m_useCase->supportsCaptureWindows());

        m_buffer->startCapture("replay-test");
        // A fonte gravada entrega tudo o mais rapido possivel; esperar o
        // suficiente evita competir com a thread de captura no primeiro pedido.
        std::this_thread::sleep_for(500ms);

        m_server.emplace(m_repository, m_useCase, m_mediaDir.string());
        ODS_CHECK(m_server->bind(0));
        ODS_CHECK(m_server->start());
        m_port = m_server->port();
        ODS_CHECK(m_port > 0);

        httplib::Client client("127.0.0.1", m_port);
        for (int attempt = 0; attempt < 80; ++attempt) {
            const auto response = client.Get("/healthz");
            if (response && response->status == 200) return;
            std::this_thread::sleep_for(25ms);
        }
        ODS_CHECK(false && "servidor HTTP nao respondeu ao healthcheck");
    }

    ~ReplayFixture() {
        if (m_server) m_server->stop();
        if (m_buffer) m_buffer->stopCapture();
        std::error_code ec;
        if (std::getenv("S4_KEEP_TEST_ARTIFACTS") != nullptr) {
            // S4_KEEP_TEST_ARTIFACTS=1 preserva os clipes para inspecao manual
            // (o smoke test depende disso para validar o MP4 com o GStreamer).
            std::cout << "  artefatos preservados em " << m_mediaDir << "\n";
            return;
        }
        std::filesystem::remove_all(m_mediaDir, ec);
    }

    [[nodiscard]] int port() const { return m_port; }

    ClipResult post_event(const std::string& eventId, const std::string& jsonFields) {
        ClipResult result;
        httplib::Client client("127.0.0.1", m_port);
        client.set_read_timeout(10s);
        client.set_write_timeout(10s);

        const std::string body = R"({"event_id":")" + eventId + R"(")" + jsonFields + "}";
        const auto response = client.Post("/api/v1/events", body, "application/json");
        ODS_CHECK(static_cast<bool>(response));
        result.status = response->status;
        result.body = response->body;
        if (result.status == 201) {
            result.clipId = json_string(result.body, "clip_id");
            ODS_CHECK(!result.clipId.empty());
        }
        return result;
    }

    ClipResult post_window_event(const std::string& eventId,
                                 std::chrono::milliseconds startMs,
                                 std::chrono::milliseconds endMs) {
        const auto result = post_event(eventId, R"(,"start_ms":)" + std::to_string(startMs.count()) +
                                           R"(,"end_ms":)" + std::to_string(endMs.count()));
        if (result.status != 201) {
            // Sem isso a falha aparece so como "posted.status == 201" e some a
            // causa real (buffer vazio, janela invalida, erro de extracao).
            std::cout << "    resposta " << result.status << ": " << result.body << "\n";
        }
        return result;
    }

    // Evento no relogio de captura: a janela e [T - pre, T + post] em ns de
    // captura, e o servidor espera o fim da janela antes de responder.
    ClipResult post_capture_event(const std::string& eventId,
                                  long long captureTsNs,
                                  double preSeconds,
                                  double postSeconds) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = post_event(
            eventId,
            R"(,"capture_ts_ns":)" + std::to_string(captureTsNs) +
                R"(,"pre_window_seconds":)" + format_double(preSeconds) +
                R"(,"post_window_seconds":)" + format_double(postSeconds)
        );
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        ).count();
        if (result.status != 201) {
            // Sem isso a falha aparece so como "posted.status == 201" e some a
            // causa real (buffer vazio, janela invalida, erro de extracao).
            std::cout << "    resposta " << result.status << " em " << elapsedMs << "ms: "
                      << result.body << "\n";
        } else {
            std::cout << "    201 em " << elapsedMs << "ms\n";
        }
        return result;
    }

    // Estado do buffer no mesmo relogio de capture_ts_ns do evento.
    BufferStatsView buffer_stats() {
        BufferStatsView view;
        httplib::Client client("127.0.0.1", m_port);
        client.set_read_timeout(10s);
        const auto response = client.Get("/api/v1/buffer/stats");
        ODS_CHECK(static_cast<bool>(response));
        ODS_CHECK(response->status == 200);
        view.has_frames = json_bool(response->body, "has_frames");
        view.oldest_capture_ts_ns = json_int(response->body, "oldest_capture_ts_ns");
        view.newest_capture_ts_ns = json_int(response->body, "newest_capture_ts_ns");
        view.span_seconds = json_double(response->body, "span_seconds");
        view.capacity_bytes = json_int(response->body, "capacity_bytes");
        view.bytes_used = json_int(response->body, "bytes_used");
        view.frames_stored = json_int(response->body, "frames_stored");
        return view;
    }

    ClipResult get(const std::string& path) {
        ClipResult result;
        httplib::Client client("127.0.0.1", m_port);
        client.set_read_timeout(10s);
        const auto response = client.Get(path);
        ODS_CHECK(static_cast<bool>(response));
        result.status = response->status;
        result.body = response->body;
        if (result.status == 200) {
            result.fileUri = json_string(result.body, "file_uri");
            result.sha256Hash = json_string(result.body, "sha256_hash");
            result.startTime = json_string(result.body, "start_time");
            result.endTime = json_string(result.body, "end_time");
        }
        return result;
    }

private:
    std::shared_ptr<infrastructure::InMemoryMediaClipRepository> m_repository;
    std::filesystem::path m_mediaDir;
    std::shared_ptr<application::ExtractClipUseCase> m_useCase;
    std::shared_ptr<infrastructure::RingBufferRAMFacade> m_buffer;
    std::optional<presentation::ClipDescriptorHttpServer> m_server;
    int m_port{0};
};

// ============================================ cenario custom (.idx/.bin)

void test_deve_registrar_clipe_com_hash_confere_quando_cenario_gravado_for_reexecutado() {
    const std::string base = generate_scenario(scenario_base(), 8);
    const std::filesystem::path basePath(base);
    ODS_CHECK(std::filesystem::exists(basePath.string() + ".idx"));
    ODS_CHECK(std::filesystem::exists(basePath.string() + ".bin"));

    ReplayFixture fixture(std::make_unique<infrastructure::RecordedFrameSource>(base));

    for (int replay = 0; replay < 3; ++replay) {
        const auto now = std::chrono::system_clock::now();
        const auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch());
        const auto posted = fixture.post_window_event(
            "evt-replay-" + std::to_string(replay), startMs,
            startMs + std::chrono::milliseconds(2000));
        ODS_CHECK(posted.status == 201);

        const auto clip = fixture.get("/api/v1/clips/" + posted.clipId);
        ODS_CHECK(clip.status == 200);
        ODS_CHECK(!clip.fileUri.empty());
        ODS_CHECK(!clip.sha256Hash.empty());
        ODS_CHECK(std::filesystem::exists(clip.fileUri));

        // A integridade probatoria: o hash do descritor e o do arquivo em disco.
        ODS_CHECK(sha256_file(clip.fileUri) == clip.sha256Hash);
        ODS_CHECK(json_bool(clip.body, "is_retained", true));
        ODS_CHECK(json_bool(clip.body, "is_locked_for_audit", false));
        // O DTO expoe timestamps ISO 8601 nos dois extremos (AAAA-MM-DDThh:mm:ss.mmmZ).
        constexpr std::size_t kIso8601Length = 24;
        ODS_CHECK(clip.startTime.size() == kIso8601Length && clip.startTime.back() == 'Z');
        ODS_CHECK(clip.endTime.size() == kIso8601Length && clip.endTime.back() == 'Z');
        ODS_CHECK(clip.startTime < clip.endTime);
    }

    std::error_code ec;
    std::filesystem::remove(basePath.string() + ".idx", ec);
    std::filesystem::remove(basePath.string() + ".bin", ec);
}

void test_deve_responder_404_quando_clipe_inexistente_for_consultado() {
    const std::string base = generate_scenario(scenario_base(), 8);
    ReplayFixture fixture(std::make_unique<infrastructure::RecordedFrameSource>(base));

    const auto clip = fixture.get("/api/v1/clips/clip-nao-existe");

    ODS_CHECK(clip.status == 404);
    std::error_code ec;
    std::filesystem::remove(base + ".idx", ec);
    std::filesystem::remove(base + ".bin", ec);
}

void test_deve_responder_400_quando_janela_invertida_for_pedida() {
    const std::string base = generate_scenario(scenario_base(), 8);
    ReplayFixture fixture(std::make_unique<infrastructure::RecordedFrameSource>(base));

    const auto posted = fixture.post_event("evt-invertida", R"(,"start_ms":2000,"end_ms":1000)");

    ODS_CHECK(posted.status == 400);
    std::error_code ec;
    std::filesystem::remove(base + ".idx", ec);
    std::filesystem::remove(base + ".bin", ec);
}

void test_deve_responder_500_quando_janela_estiver_fora_do_buffer() {
    const std::string base = generate_scenario(scenario_base(), 8);
    ReplayFixture fixture(std::make_unique<infrastructure::RecordedFrameSource>(base));

    // Uma hora no futuro nao tem quadros no buffer: o S4.2 vira
    // MediaBufferEmptyError, que o servidor mapeia para 500.
    const auto future = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch());
    const auto posted = fixture.post_window_event(
        "evt-futuro", future, future + std::chrono::milliseconds(1000));

    ODS_CHECK(posted.status == 500);
    std::error_code ec;
    std::filesystem::remove(base + ".idx", ec);
    std::filesystem::remove(base + ".bin", ec);
}

// ============================================ cenario MP4 real (com GStreamer)

#ifdef ODS_S4_WITH_GSTREAMER

// O primeiro .mp4 encontrado em video_test/ — o cenario de video real.
std::optional<std::filesystem::path> find_sample_mp4() {
    const std::filesystem::path dir("video_test");
    if (!std::filesystem::exists(dir)) return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
            return entry.path();
        }
    }
    return std::nullopt;
}

// Codigo de saida do cenario de video real: 0 rodou, 77 nao ha material.
//
// Sem o .mp4 em video_test/ o cenario nao pode rodar, e o que ele prova (o
// fluxo completo sobre video real) e exatamente o que fica sem prova. Antes
// isso era um "return" silencioso e o ctest reportava aprovado — o mesmo
// buraco que o aviso de "compilado sem GStreamer" agora cobre: um teste que
// nao exercita o que diz exercitar precisa aparecer como pulado, nunca como
// aprovado. Da para quem compile com -DODS_S4_REQUIRE_SAMPLE_VIDEO(=ON) exigir
// o material: e o caso do build Docker, que tem o video.
int test_deve_registrar_clipe_decodificavel_quando_video_real_for_reproduzido() {
    const auto mp4 = find_sample_mp4();
    if (!mp4.has_value()) {
        std::cout << "  video_test/ sem MP4 — cenario de video real pulado\n";
        return 77;
    }
    std::cout << "  cenario MP4: " << mp4->string() << "\n";

    auto source = std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
        infrastructure::GStreamerAppsinkFrameSource::fileReplayH264Pipeline(mp4->string()),
        "replay-mp4-1");

    ReplayFixture fixture(std::move(source));
    std::this_thread::sleep_for(1500ms);

    // A janela e pedida no RELOGIO DE CAPTURA, e nao no relogio de parede. Com
    // filesrc sync=false o replay avanca o video o mais rapido possivel, entao
    // uma janela em system_clock::now() cairia fora do buffer e o pedido
    // voltaria 500 sem frames. O instante do evento vem do proprio buffer.
    const auto stats = fixture.buffer_stats();
    ODS_CHECK(stats.has_frames);
    ODS_CHECK(stats.newest_capture_ts_ns > 0);
    std::cout << "  capture_ts_ns disponivel: ["
              << stats.oldest_capture_ts_ns << ", " << stats.newest_capture_ts_ns << "]"
              << " span=" << stats.span_seconds << "s"
              << " | frames=" << stats.frames_stored
              << " bytes=" << stats.bytes_used << "/" << stats.capacity_bytes << "\n";
    // O clock precisa cobrir um intervalo, senao nao ha janela enderecavel.
    ODS_CHECK(stats.newest_capture_ts_ns > stats.oldest_capture_ts_ns);
    // E precisa caber na capacidade declarada: um buffer saturado que ainda
    // afirma guardar 69 s seria uma promessa falsa.
    ODS_CHECK(stats.bytes_used <= stats.capacity_bytes);

    // O instante do evento e escolhido DENTRO da linha do tempo gravada, e
    // nao "agora": o replay de arquivo entrega o video inteiro em centenas de
    // milissegundos e depois chega ao EOS, entao nao existe futuro no relogio
    // de captura. Pedir agora + post_windowSeconds esperaria para sempre.
    //
    // E essa a raza de a janela precisar ser endereçada em relogio de captura:
    // e assim que se aponta para um instante do video que ja passou.
    const long long eventTsNs = stats.newest_capture_ts_ns -
        static_cast<long long>(0.5 * domain::kNanosecondsPerSecond);
    // O "antes" precisa estar no buffer e o "depois" tem de ser o proprio
    // instante do evento: um pos-evento alem do fim do arquivo nao existe.
    constexpr double kPreSeconds = 0.5;
    constexpr double kPostSeconds = 0.0;

    const auto posted = fixture.post_capture_event(
        "evt-mp4-1", eventTsNs, kPreSeconds, kPostSeconds
    );
    ODS_CHECK(posted.status == 201);
    if (posted.status != 201) {
        return 1;
    }

    const auto clip = fixture.get("/api/v1/clips/" + posted.clipId);
    ODS_CHECK(clip.status == 200);
    ODS_CHECK(sha256_file(clip.fileUri) == clip.sha256Hash);

    // O arquivo tem que ser um MP4 de verdade: a assinatura do container
    // ISO-BMFF e o campo ftyp. Um elementary stream Annex-B com extensao .mp4
    // nao tem nenhum dos dois.
    const auto bytes = static_cast<std::uintmax_t>(std::filesystem::file_size(clip.fileUri));
    ODS_CHECK(bytes > 10000);
    ODS_CHECK(file_contains(clip.fileUri, "ftyp"));
    std::cout << "  clipe MP4: " << bytes << " bytes, contem ftyp\n";
    return 0;
}

#endif // ODS_S4_WITH_GSTREAMER

} // namespace

int main() {
    std::cout << "test_s4_replay_e2e: cenario custom\n";
    test_deve_registrar_clipe_com_hash_confere_quando_cenario_gravado_for_reexecutado();
    test_deve_responder_404_quando_clipe_inexistente_for_consultado();
    test_deve_responder_400_quando_janela_invertida_for_pedida();
    test_deve_responder_500_quando_janela_estiver_fora_do_buffer();

    // 0 = o cenario de video real rodou; 77 = nao rodou (sem GStreamer ou sem
    // MP4 em video_test/). As duas coisas sao ausencias de material, e nenhuma
    // delas pode virar "aprovado".
    int cenarioVideoReal = 0;
#ifdef ODS_S4_WITH_GSTREAMER
    std::cout << "test_s4_replay_e2e: cenario MP4\n";
    cenarioVideoReal =
        test_deve_registrar_clipe_decodificavel_quando_video_real_for_reproduzido();
#else
    // Ausencia de GStreamer e uma escolha explicita agora (o CMake falha o
    // configure por padrao), mas mesmo assim este teste nao pode sumir em
    // silencio: "all tests passed" sem nenhuma verificacao de MP4 real e
    // exatamente o que deixou uma imagem Docker inteira passar sem reproduzir
    // um quadro. O aviso vai para o log do ctest, nao para o limbo.
    std::cout << "\n*** AVISO: compilado SEM GStreamer; o cenario de MP4 real "
                 "(replay, mux, decodificabilidade, hash) NAO foi executado. "
                 "Estes testes passarão sem exercitar essa parte. ***\n\n";
    cenarioVideoReal = 77;
#endif

    if (cenarioVideoReal != 0) {
#ifdef ODS_S4_REQUIRE_SAMPLE_VIDEO
        std::cerr << "test_s4_replay_e2e: ODS_S4_REQUIRE_SAMPLE_VIDEO esta ON e o "
                     "cenario de MP4 real nao foi executado — reprovando em vez "
                     "de declarar o material opcional.\n";
        return 1;
#else
        std::cout << "test_s4_replay_e2e: cenario de video real NAO executado "
                     "(77 = skipped)\n";
        return 77;
#endif
    }

    std::cout << "test_s4_replay_e2e: all tests passed\n";
    return 0;
}
