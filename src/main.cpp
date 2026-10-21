#include "s4/application/use_cases/extract_clip.hpp"
#include "s4/application/use_cases/purge_media.hpp"
#include "s4/domain/strategies/disk_quota_fifo_strategy.hpp"
#include "s4/domain/strategies/lgpd_retention_strategy.hpp"
#include "s4/infrastructure/database/sqlite_media_clip_repository.hpp"
#include "s4/infrastructure/filesystem/file_storage.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/retention/json_lines_purge_log.hpp"
#include "s4/infrastructure/retention/purge_daemon.hpp"
#include "s4/infrastructure/retention/statvfs_disk_usage_provider.hpp"
#include "s4/infrastructure/retention/system_clock.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"
#include "s4/presentation/http/clip_descriptor_http_server.hpp"

#if ODS_S4_WITH_GSTREAMER
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/gstreamer_mp4_muxer.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;

void on_signal(int) {
    g_stopRequested = 1;
}

void print_usage(const char* program) {
    std::cerr << "usage: " << program
              << " [--port PORT] [--db PATH] [--media-dir PATH]\n"
                 "       [--source FILE] [--scenario] [--camera ID]\n"
                 "       [--window-seconds N] [--bitrate BPS]\n"
                 "       [--retention-days DAYS] [--quota-trigger RATIO] [--quota-target RATIO]\n"
                 "       [--sweep-interval SECONDS] [--disk-check-interval SECONDS] [--purge-log PATH]\n"
                 "\n"
                 "  --source FILE     replay de um MP4 H.264 existente, em vez da\n"
                 "                    fonte sintetica. E o caminho que produz um clipe\n"
                 "                    MP4 de verdade a partir de video de verdade.\n"
                 "  --scenario        extrai um clipe de demonstracao na subida e\n"
                 "                    imprime o descriptor. Sem este flag o daemon so\n"
                 "                    serve a API.\n"
                 "  --camera ID        id da camera (padrao: cam0)\n"
                 "  --window-seconds N  segundos de ring buffer (padrao: 30)\n"
                 "  --bitrate BPS     bitrate nominal usado para dimensionar o\n"
                 "                    ring buffer (padrao: 134400)\n"
                 "\n"
                 "  --retention-days N   dias que a midia fica guardada (padrao: 7, LGPD)\n"
                 "  --quota-trigger R     ocupacao do NVMe que dispara o expurgo (padrao: 0.85)\n"
                 "  --quota-target R      ocupacao a que o expurgo volta (padrao: 0.80)\n"
                 "  --sweep-interval N      segundos entre varreduras de prazo (padrao: 3600)\n"
                 "  --disk-check-interval N  segundos entre consultas de statvfs (padrao: 30)\n"
                 "  --purge-log PATH        log JSONL do expurgo (padrao: ao lado do banco)\n";
}

// S4.3 — parametros do expurgo. Os padroes sao os do guia: 7 dias (LGPD) e
// gatilho de 85% do NVMe.
struct RetentionOptions {
    double retentionDays{7.0};
    double quotaTrigger{ods::s4::domain::QuotaPolicy::kDefaultTriggerRatio};
    double quotaTarget{ods::s4::domain::QuotaPolicy::kDefaultTargetRatio};
    long long sweepIntervalSeconds{3600};
    long long diskCheckIntervalSeconds{30};
    std::string purgeLogPath;
};

// Os dois shared_ptr chegam POR COPIA e sao movidos para dentro do caso de uso.
// O chamador tambem precisa deles: o extrator de clipes usa o mesmo repositorio
// e o mesmo FileStorage, e um std::move no call site deixaria o extrator com
// ponteiro nulo — sintoma: expurgo funcionando e API sem responder.
std::unique_ptr<ods::s4::infrastructure::PurgeDaemon> make_purge_daemon(
    const RetentionOptions& options,
    std::shared_ptr<ods::s4::domain::IMediaClipRepository> repository,
    std::shared_ptr<ods::s4::domain::IFileStorage> fileStorage,
    const std::string& mediaDir
) {
    using namespace ods::s4;
    const domain::RetentionPolicy retention(std::chrono::seconds(
        static_cast<long long>(options.retentionDays * 24 * 3600)));
    const domain::QuotaPolicy quota(options.quotaTrigger, options.quotaTarget);
    auto diskUsage = std::make_shared<infrastructure::StatvfsDiskUsageProvider>(mediaDir);

    auto purgeMedia = std::make_shared<application::PurgeMediaUseCase>(
        std::move(repository), std::move(fileStorage), diskUsage,
        std::make_shared<infrastructure::JsonLinesPurgeLog>(options.purgeLogPath),
        std::make_shared<infrastructure::SystemClock>(),
        std::vector<std::shared_ptr<const domain::IRetentionStrategy>>{
            std::make_shared<domain::LgpdRetentionStrategy>(retention),
            std::make_shared<domain::DiskQuotaFifoStrategy>(quota),
        });

    infrastructure::PurgeDaemonConfig config;
    config.retentionSweepInterval = std::chrono::seconds(options.sweepIntervalSeconds);
    config.diskCheckInterval = std::chrono::seconds(options.diskCheckIntervalSeconds);
    return std::make_unique<infrastructure::PurgeDaemon>(purgeMedia, diskUsage, quota, config);
}

// Quanto tempo o daemon espera a ingestao chegar num estado utilizavel antes
// de desistir. Em replay de arquivo isso e irrelevante (o arquivo inteiro cabe
// em centenas de milissegundos); numa camera real e o tempo que ela leva para
// encher a janela.
constexpr int kCaptureSettleTimeoutSeconds = 30;

// Quantas medicoes consecutivas precisam concordar para a ingestao ser
// considerada parada. Tres leituras com 100 ms de intervalo: um stream que
// parou e um stream que engasgou por mais de um quarto de segundo nao sao a
// mesma coisa, e esperar um unico zero seria correr.
constexpr int kStablePollsRequired = 3;
constexpr int kPollIntervalMs = 100;

// Espera o ring buffer ter material suficiente E, quando a fonte e finita,
// parar de crescer.
//
// A segunda condicao so se aplica ao replay de arquivo, e ai ela nao e um
// detalhe: o replay entrega os 10 minutos de video em ~500 ms, entao o primeiro
// poll pode encontrar o buffer ainda enchendo, com o "instante mais recente"
// parado no meio da gravacao. O cenario ancoraria num ponto que muda de
// execucao para execucao e produziria um clipe diferente a cada rodada — o smoke
// test acusaria hash instavel sem que nada estivesse obviamente quebrado.
//
// Numa fonte continua (camera ao vivo ou a sintetica do demo) a ingestao nunca
// para, e exigir que pare seria exigir o impossivel: o cenário ficaria 30
// segundos esperando e depois desistiria sem extrair nada. E o deadline que
// governa, porque o que importa ali e "ja tenho N segundos", nao "ja terminou".
bool wait_for_capture_to_settle(
    const std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade>& mediaBuffer,
    double minSpanSeconds,
    bool sourceIsFinite
) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kCaptureSettleTimeoutSeconds);
    std::uint64_t previousIngested = 0;
    double previousSpan = -1.0;
    int stablePolls = 0;

    while (std::chrono::steady_clock::now() < deadline && g_stopRequested == 0) {
        const auto stats = mediaBuffer->bufferStats();
        if (stats.hasFrames && stats.spanSeconds() >= minSpanSeconds) {
            if (!sourceIsFinite) {
                // So ter material basta: a janela [T - pre, T] que o cenario
                // pede ja esta no buffer, e o "depois do evento" e o proprio
                // instante mais recente.
                return true;
            }
            if (stats.framesIngestedTotal == previousIngested &&
                stats.spanSeconds() == previousSpan) {
                ++stablePolls;
            } else {
                stablePolls = 0;
            }
            previousIngested = stats.framesIngestedTotal;
            previousSpan = stats.spanSeconds();
            if (stablePolls >= kStablePollsRequired) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    return false;
}

// Extrai um clipe de demonstracao e imprime o descriptor.
//
// O instante do evento e escolhido DENTRO do intervalo que o ring buffer
// realmente guarda, e nao "agora". Isso nao e um detalhe: em replay de arquivo
// o video inteiro e consumido em centenas de milissegundos e o processo chega
// ao EOS, depois do qual o relogio de captura CONGELA. Um cenario ancorado no
// instante mais recente pediria um "depois do evento" que nunca existe, e a
// extracao ficaria esperando ate o timeout — foi exatamente assim que a suite
// E2E ficou flaky antes de o instante passar a ser derivado do buffer.
//
// Em camera real o relogio de captura continua avancando, e ancorar no buffer
// continua correto: a janela [T - pre, T] e exatamente o que se quer guardar
// em torno de um evento.
void run_demo_scenario(
    const std::shared_ptr<ods::s4::application::ExtractClipUseCase>& extractClip,
    const std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade>& mediaBuffer,
    const std::string& mediaDir,
    int port,
    bool sourceIsFinite
) {
    using ods::s4::domain::kNanosecondsPerSecond;

    constexpr double kPreSeconds = 1.0;
    constexpr double kPostSeconds = 0.5;

    const double needed = kPreSeconds + kPostSeconds;
    if (sourceIsFinite) {
        std::cout << "Waiting for the ring buffer to hold at least " << needed
                  << " s and stop growing...\n";
    } else {
        std::cout << "Waiting for the ring buffer to hold at least " << needed
                  << " s...\n";
    }
    std::cout.flush();

    if (!wait_for_capture_to_settle(mediaBuffer, needed, sourceIsFinite)) {
        const auto stats = mediaBuffer->bufferStats();
        std::cerr << "ring buffer never settled holding " << needed << " s of video"
                  << " (has_frames=" << (stats.hasFrames ? "yes" : "no")
                  << ", span=" << stats.spanSeconds() << " s, ingested="
                  << stats.framesIngestedTotal << ", rejected-by-domain=see log)\n";
        return;
    }

    const auto stats = mediaBuffer->bufferStats();
    std::cout << "buffer: " << stats.framesStored << " frames, " << stats.bytesUsed
              << "/" << stats.capacityBytes << " bytes, span " << stats.spanSeconds()
              << " s, session " << stats.sessionId << '\n';
    std::cout.flush();

    // Fim da janela = instante mais recente guardado. Assim o "depois do
    // evento" ja chegou e a extracao nao precisa esperar.
    const auto eventTsNs = stats.newestCaptureTsNs -
        static_cast<ods::s4::domain::Nanoseconds>(kPostSeconds * kNanosecondsPerSecond);
    const auto window = ods::s4::domain::CaptureWindow::around(
        eventTsNs, kPreSeconds, kPostSeconds
    );

    const std::string outputPath =
        (std::filesystem::path(mediaDir) / "event-demo.mp4").string();
    try {
        const auto clip = extractClip->executeCaptureWindow(
            "event-demo", window, outputPath
        );
        std::cout << "clip_id: " << clip.clipId() << '\n';
        std::cout << "file:    " << clip.filePath() << '\n';
        std::cout << "sha256:  " << clip.sha256Hash() << '\n';
        std::cout << "GET http://127.0.0.1:" << port << "/api/v1/clips/"
                  << clip.clipId() << "\n";
        std::cout.flush();
    } catch (const std::exception& error) {
        std::cerr << "clip extraction failed: " << error.what() << '\n';
        if (!sourceIsFinite) {
            // A fonte sintetica alimenta o ring buffer com bytes que NAO sao
            // H.264 codificado: existe para exercitar janela, eviccao e
            // alinhamento a keyframe, nao para virar container. O muxer
            // (appsrc ! h264parse ! mp4mux) nao tem o que parsear, e o
            // resultado seria um arquivo que so parece MP4. Dizer isso e
            // melhor que devolver um .mp4 invalido e chamar de evidencia.
            std::cerr << "a fonte sintetica nao produz H.264 real, entao nao ha "
                         "container a extrair aqui; para o clipe de video de "
                         "verdade, rode com --source <arquivo.mp4>\n";
        }
    }
}

} // namespace


int main(int argc, char* argv[]) {
    int port = 8080;
    std::string dbPath = "/tmp/s4_media_evidence.db";
    std::string mediaDir = "/tmp/s4_media_evidence";
    // S4.1/S4.4 — captura e replay.
    std::string sourcePath;
    std::string cameraId = "cam0";
    double windowSeconds = 30.0;
    long long bitrateBps = 134400;
    bool runScenario = false;
    // S4.3 — expurgo.
    RetentionOptions retentionOptions;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto read_value = [&](const char* flag) -> bool {
                if (argument == flag && i + 1 < argc) {
                    ++i;
                    return true;
                }
                return false;
            };
            if (read_value("--port")) {
                port = std::stoi(argv[i]);
            } else if (read_value("--db")) {
                dbPath = argv[i];
            } else if (read_value("--media-dir")) {
                mediaDir = argv[i];
            } else if (read_value("--source")) {
                sourcePath = argv[i];
            } else if (read_value("--camera")) {
                cameraId = argv[i];
            } else if (read_value("--window-seconds")) {
                windowSeconds = std::stod(argv[i]);
            } else if (read_value("--bitrate")) {
                bitrateBps = std::stoll(argv[i]);
            } else if (argument == "--scenario") {
                runScenario = true;
            } else if (read_value("--retention-days")) {
                retentionOptions.retentionDays = std::stod(argv[i]);
            } else if (read_value("--quota-trigger")) {
                retentionOptions.quotaTrigger = std::stod(argv[i]);
            } else if (read_value("--quota-target")) {
                retentionOptions.quotaTarget = std::stod(argv[i]);
            } else if (read_value("--sweep-interval")) {
                retentionOptions.sweepIntervalSeconds = std::stoll(argv[i]);
            } else if (read_value("--disk-check-interval")) {
                retentionOptions.diskCheckIntervalSeconds = std::stoll(argv[i]);
            } else if (read_value("--purge-log")) {
                retentionOptions.purgeLogPath = argv[i];
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }
    } catch (const std::logic_error&) {
        // std::stoi/stod com texto nao numerico ou fora da faixa.
        print_usage(argv[0]);
        return 1;
    }
    if (retentionOptions.sweepIntervalSeconds <= 0 || retentionOptions.diskCheckIntervalSeconds <= 0) {
        std::cerr << "--sweep-interval and --disk-check-interval must be positive\n";
        return 1;
    }
    if (retentionOptions.purgeLogPath.empty()) {
        // Ao lado do banco: no container, os dois ficam no volume /data.
        retentionOptions.purgeLogPath =
            (std::filesystem::path(dbPath).parent_path() / "s4_purge_log.jsonl").string();
    }

    if (!sourcePath.empty() && !std::filesystem::is_regular_file(sourcePath)) {
        std::cerr << "--source is not a readable file: '" << sourcePath << "'\n";
        return 1;
    }

    std::error_code errorCode;
    std::filesystem::create_directories(mediaDir, errorCode);
    if (errorCode) {
        std::cerr << "cannot create media directory '" << mediaDir << "': "
                  << errorCode.message() << '\n';
        return 1;
    }

    auto repository = std::make_shared<ods::s4::infrastructure::SQLiteMediaClipRepository>(dbPath);

    // S4.1 — ring buffer real em /dev/shm. A fonte sintetica mantem o demo
    // rodando sem camera; com --source, um MP4 real e replayado. Nada mais
    // nesta funcao muda: o ExtractClipUseCase so conhece a porta
    // IMediaBufferReader, entao trocar a fonte nao toca no resto do fluxo.
    ods::s4::application::RingBufferConfig bufferConfig;
    bufferConfig.cameraId = cameraId;
    bufferConfig.windowSeconds = windowSeconds;
    bufferConfig.bitrateBps = bitrateBps;
    bufferConfig.width = 320;
    bufferConfig.height = 240;
    // Daemon: apos uma queda, o /dev/shm da execucao anterior pode ter ficado
    // para tras e impediria a subida. Uma instancia por camera, entao limpar e
    // o comportamento correto aqui.
    bufferConfig.replaceStaleSegment = true;

    std::unique_ptr<ods::s4::domain::IFrameSource> frameSource;
    std::string sessionId;

    if (sourcePath.empty()) {
        ods::s4::infrastructure::SyntheticSourceOptions sourceOptions;
        sourceOptions.fps = 15.0;
        sourceOptions.gop = 15;
        sourceOptions.keyframeBytes = 6000;
        sourceOptions.deltaBytes = 1200;
        sourceOptions.sessionId = "exec-demo-1";
        sessionId = sourceOptions.sessionId;
        frameSource = std::make_unique<ods::s4::infrastructure::SyntheticFrameSource>(sourceOptions);
    } else {
#if ODS_S4_WITH_GSTREAMER
        // A fonte de replay e marcada com um id de sessao proprio: o ring
        // buffer invalida o conteudo anterior quando a sessao muda, e um id
        // estavel distingue "restartei a mesma fonte" de "comecei outra
        // gravacao". O id deriva do proprio arquivo, entao dois daemons do
        // mesmo video nao se confundem.
        sessionId = "replay-" +
            std::filesystem::path(sourcePath).filename().string();
        frameSource = std::make_unique<ods::s4::infrastructure::GStreamerAppsinkFrameSource>(
            ods::s4::infrastructure::GStreamerAppsinkFrameSource::fileReplayH264Pipeline(
                sourcePath
            ),
            sessionId
        );
#else
        std::cerr << "--source requires GStreamer; rebuild with -DODS_S4_WITH_GSTREAMER=ON\n";
        return 1;
#endif
    }

    std::shared_ptr<ods::s4::infrastructure::RingBufferRAMFacade> mediaBuffer;
    try {
        mediaBuffer = std::make_shared<ods::s4::infrastructure::RingBufferRAMFacade>(
            std::move(bufferConfig), std::move(frameSource)
        );
    } catch (const ods::s4::domain::ODSBaseException& error) {
        std::cerr << "cannot start the S4.1 ring buffer: " << error.what() << std::endl;
        return 1;
    }
    auto fileStorage = std::make_shared<ods::s4::infrastructure::FileStorage>();

    // As duas portas do fluxo real (janela em relogio de captura + mux para MP4)
    // sao injetadas aqui, e nao dentro do caso de uso. Sem GStreamer elas ficam
    // nulas: o daemon continua servindo a API e executeCaptureWindow falha com
    // MediaMuxError, em vez de inventar um container.
    std::shared_ptr<ods::s4::application::IMediaMuxer> muxer;
#if ODS_S4_WITH_GSTREAMER
    muxer = std::make_shared<ods::s4::infrastructure::GStreamerMp4Muxer>();
#endif

    auto extractClip = std::make_shared<ods::s4::application::ExtractClipUseCase>(
        repository, mediaBuffer, fileStorage, mediaBuffer, muxer
    );

    // S4.3 — expurgo automatico (LGPD + cota do NVMe), em thread propria.
    // repository e fileStorage sao entregues por copia: o extrator de clipes
    // usa os mesmos dois.
    std::unique_ptr<ods::s4::infrastructure::PurgeDaemon> purgeDaemon;
    try {
        purgeDaemon = make_purge_daemon(retentionOptions, repository, fileStorage, mediaDir);
    } catch (const ods::s4::domain::ODSBaseException& error) {
        std::cerr << "invalid retention settings: " << error.what() << std::endl;
        return 1;
    }

    mediaBuffer->startCapture(sessionId);
    purgeDaemon->start();

    ods::s4::presentation::ClipDescriptorHttpServer server(repository, extractClip, mediaDir);
    if (!server.bind(port)) {
        std::cerr << "cannot bind to port " << port << '\n';
        return 1;
    }
    port = server.port();
    if (!server.start()) {
        std::cerr << "cannot start HTTP server\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "S4 Media Evidence listening on http://127.0.0.1:" << port
              << "/api/v1/clips/{id}\n";
    std::cout << "Simulate an event: POST http://127.0.0.1:" << port
              << "/api/v1/events\n";
    std::cout << "Purge daemon: retention " << retentionOptions.retentionDays << " d, quota "
              << retentionOptions.quotaTrigger * 100 << "% -> " << retentionOptions.quotaTarget * 100
              << "%, log " << retentionOptions.purgeLogPath << '\n';
    if (!sourcePath.empty()) {
        std::cout << "Replaying '" << sourcePath << "' into the ring buffer\n";
    } else {
        std::cout << "Recording synthetic feed\n";
    }
    std::cout.flush();

    if (runScenario) {
        // So o replay de arquivo tem fim; a fonte sintetica e a camera seguem
        // ingerindo para sempre, e exigir que pare deixaria o demo sem clipe.
        run_demo_scenario(extractClip, mediaBuffer, mediaDir, port, !sourcePath.empty());
    }

    std::cout << "Idle until SIGINT...\n";
    std::cout.flush();

    while (g_stopRequested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    purgeDaemon->stop();
    mediaBuffer->stopCapture();
    server.stop();
    return 0;
}