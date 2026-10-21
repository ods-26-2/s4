#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/domain/services/frame_source.hpp"
#include "s4/infrastructure/ringbuffer/frame_log.hpp"
#include "s4/infrastructure/ringbuffer/synthetic_frame_source.hpp"

#ifdef ODS_S4_WITH_GSTREAMER
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#endif

using namespace ods::s4;
using namespace std::chrono_literals;

enum class SourceType { Synthetic, GStreamerTestPattern, GStreamerWebcam, GStreamerShm };

struct CliOptions {
    SourceType source = SourceType::Synthetic;
    std::string outputBase;
    std::string scenarioName = "evento_porta";
    double durationSec = 30.0;
    double fps = 15.0;
    int width = 1280;
    int height = 720;
    int bitrateKbps = 2000;
    int gop = 30;
    std::string shmPath = "/dev/shm/ods_p4_cam0";
    std::string webcamDevice = "/dev/video0";
    bool realtime = false;
    size_t maxFrames = 0;
};

void printUsage(const char* prog) {
    std::cerr << "Uso: " << prog << " [opcoes]\n\n"
              << "Grava cenarios de video para os testes do S4, no formato custom\n"
              << "texto-indice + blob binario (.idx/.bin), que o RecordedFrameSource\n"
              << "reproduz sem camera e sem GStreamer.\n\n"
              << "Para cenarios .mp4 use uma fonte externa (gravador do P6, ou um\n"
              << "arquivo de teste): o daemon os consome direto por --source replay-mp4.\n\n"
              << "Opcoes:\n";
    std::cerr << "  --source <tipo>        Fonte: synthetic | gst-test | gst-webcam | gst-shm (padrao: synthetic)\n";
    std::cerr << "  --out <caminho>        Base do arquivo saida (sem extensao). Padrao: ./scenarios/<scenario>\n";
    std::cerr << "  --scenario <nome>      Nome do cenario (usado se --out nao dado). Padrao: evento_porta\n";
    std::cerr << "  --duration <seg>       Duracao da gravacao em segundos (padrao: 30)\n";
    std::cerr << "  --fps <valor>          FPS (padrao: 15)\n";
    std::cerr << "  --width <px>           Largura (padrao: 1280)\n";
    std::cerr << "  --height <px>          Altura (padrao: 720)\n";
    std::cerr << "  --bitrate <kbps>       Bitrate alvo em kbps (padrao: 2000)\n";
    std::cerr << "  --gop <valor>          Intervalo de keyframes (padrao: 30)\n";
    std::cerr << "  --shm-path <path>      Caminho shm para gst-shm (padrao: /dev/shm/ods_p4_cam0)\n";
    std::cerr << "  --webcam <device>      Device webcam para gst-webcam (padrao: /dev/video0)\n";
    std::cerr << "  --realtime             Respeita FPS real (padrao: maximo possivel)\n";
    std::cerr << "  --max-frames <n>       Limite de quadros (0 = ilimitado, padrao: 0)\n";
    std::cerr << "  -h, --help             Mostra esta ajuda\n";
}

CliOptions parseArgs(int argc, char* argv[]) {
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            std::exit(0);
        } else if (arg == "--source" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "synthetic") opts.source = SourceType::Synthetic;
            else if (v == "gst-test") opts.source = SourceType::GStreamerTestPattern;
            else if (v == "gst-webcam") opts.source = SourceType::GStreamerWebcam;
            else if (v == "gst-shm") opts.source = SourceType::GStreamerShm;
            else { std::cerr << "Fonte desconhecida: " << v << "\n"; std::exit(1); }
        } else if (arg == "--out" && i + 1 < argc) {
            opts.outputBase = argv[++i];
        } else if (arg == "--scenario" && i + 1 < argc) {
            opts.scenarioName = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            opts.durationSec = std::stod(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            opts.fps = std::stod(argv[++i]);
        } else if (arg == "--width" && i + 1 < argc) {
            opts.width = std::stoi(argv[++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            opts.height = std::stoi(argv[++i]);
        } else if (arg == "--bitrate" && i + 1 < argc) {
            opts.bitrateKbps = std::stoi(argv[++i]);
        } else if (arg == "--gop" && i + 1 < argc) {
            opts.gop = std::stoi(argv[++i]);
        } else if (arg == "--shm-path" && i + 1 < argc) {
            opts.shmPath = argv[++i];
        } else if (arg == "--webcam" && i + 1 < argc) {
            opts.webcamDevice = argv[++i];
        } else if (arg == "--realtime") {
            opts.realtime = true;
        } else if (arg == "--max-frames" && i + 1 < argc) {
            opts.maxFrames = std::stoull(argv[++i]);
        } else {
            std::cerr << "Opcao desconhecida: " << arg << "\n";
            printUsage(argv[0]);
            std::exit(1);
        }
    }
    return opts;
}

std::unique_ptr<domain::IFrameSource> createSource(const CliOptions& opts) {
    switch (opts.source) {
        case SourceType::Synthetic: {
            infrastructure::SyntheticSourceOptions sopt;
            sopt.fps = opts.fps;
            sopt.gop = opts.gop;
            sopt.keyframeBytes = (opts.bitrateKbps * 1000 / 8) / opts.fps * 3; // ~3x frame medio
            sopt.deltaBytes = sopt.keyframeBytes / 5;
            sopt.sessionId = opts.scenarioName + "-session";
            sopt.realtime = opts.realtime;
            sopt.maxFrames = opts.maxFrames ? opts.maxFrames : static_cast<size_t>(opts.durationSec * opts.fps);
            return std::make_unique<infrastructure::SyntheticFrameSource>(sopt);
        }
#ifdef ODS_S4_WITH_GSTREAMER
        case SourceType::GStreamerTestPattern: {
            auto pipeline = infrastructure::GStreamerAppsinkFrameSource::testPatternH264Pipeline(
                opts.width, opts.height, static_cast<int>(opts.fps), opts.bitrateKbps, opts.gop
            );
            return std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
                pipeline, opts.scenarioName + "-gst-test"
            );
        }
        case SourceType::GStreamerWebcam: {
            auto pipeline = "v4l2src device=" + opts.webcamDevice + " ! "
                          "video/x-h264,width=" + std::to_string(opts.width) + ",height=" + std::to_string(opts.height)
                          + ",framerate=" + std::to_string(static_cast<int>(opts.fps)) + "/1 ! "
                          "h264parse config-interval=-1 ! appsink sync=false drop=false max-buffers=0";
            return std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
                pipeline, opts.scenarioName + "-webcam"
            );
        }
        case SourceType::GStreamerShm: {
            auto pipeline = infrastructure::GStreamerAppsinkFrameSource::shmH264Pipeline(opts.shmPath);
            return std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
                pipeline, opts.scenarioName + "-shm"
            );
        }
#else
        case SourceType::GStreamerTestPattern:
        case SourceType::GStreamerWebcam:
        case SourceType::GStreamerShm:
            std::cerr << "GStreamer nao disponivel (compile com ODS_S4_WITH_GSTREAMER=ON)\n";
            std::exit(1);
#endif
    }
    return nullptr;
}

template <typename ConcreteSource>
void waitForSource(ConcreteSource* source) {
    if constexpr (requires { source->waitUntilFinished(); }) {
        source->waitUntilFinished();
    } else {
        // Fallback: wait based on duration estimate
        std::this_thread::sleep_for(1s);
    }
}

void recordScenario(const CliOptions& opts, std::unique_ptr<domain::IFrameSource>& source) {
    infrastructure::FrameLogRecorder recorder(opts.outputBase);
    source->start([&recorder](const domain::CapturedFrame& frame) { recorder.record(frame); });
    
    // Try to call waitUntilFinished on concrete type
    if (auto* synth = dynamic_cast<infrastructure::SyntheticFrameSource*>(source.get())) {
        synth->waitUntilFinished();
    } else if (auto* recorded = dynamic_cast<infrastructure::RecordedFrameSource*>(source.get())) {
        recorded->waitUntilFinished();
    }
#ifdef ODS_S4_WITH_GSTREAMER
    else if (dynamic_cast<infrastructure::GStreamerAppsinkFrameSource*>(source.get()) != nullptr) {
        // A fonte de GStreamer nao tem waitUntilFinished: um replay de arquivo
        // acaba no EOS e uma camera nao acaba nunca, entao espera-se a duracao
        // pedida mais uma folga.
        std::this_thread::sleep_for(std::chrono::duration<double>(opts.durationSec + 2.0));
    }
#endif
    else {
        std::this_thread::sleep_for(std::chrono::duration<double>(opts.durationSec + 2.0));
    }
    
    recorder.close();
    std::cout << "Gravado: " << opts.outputBase << ".idx + " << opts.outputBase << ".bin\n";
}

int main(int argc, char* argv[]) {
    CliOptions opts = parseArgs(argc, argv);

    if (opts.outputBase.empty()) {
        std::filesystem::create_directories("scenarios");
        opts.outputBase = "scenarios/" + opts.scenarioName;
    }

    std::cout << "=== S4 Record Scenario ===\n";
    std::cout << "Fonte: ";
    switch (opts.source) {
        case SourceType::Synthetic: std::cout << "Sintetica"; break;
        case SourceType::GStreamerTestPattern: std::cout << "GStreamer Test Pattern"; break;
        case SourceType::GStreamerWebcam: std::cout << "GStreamer Webcam"; break;
        case SourceType::GStreamerShm: std::cout << "GStreamer SHM"; break;
    }
    std::cout << "\nFormato: custom (.idx/.bin)\n";
    std::cout << "Saida: " << opts.outputBase << "\n";
    std::cout << "Duracao: " << opts.durationSec << "s, FPS: " << opts.fps << ", " << opts.width << "x" << opts.height << "\n";
    std::cout << "============================\n\n";

    auto source = createSource(opts);
    if (!source) {
        std::cerr << "Falha ao criar fonte\n";
        return 1;
    }

    try {
        recordScenario(opts, source);
    } catch (const std::exception& e) {
        std::cerr << "Erro: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\nGravacao concluida com sucesso.\n";
    return 0;
}