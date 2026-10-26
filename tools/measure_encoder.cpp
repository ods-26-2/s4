// S4.1 item 1 — mede o custo do encoder por software NA PLACA.
//
// A Orin Nano nao tem NVENC: o S4 codifica com x264enc, dividindo a CPU com a
// inferencia. O ponto de partida da NVIDIA (18-22% de um nucleo a 1080p30,
// 10 Mbps, GOP 30, sem B-frames, presets ultrafast a veryfast) precisa virar
// numero medido aqui, com a inferencia rodando junto.
//
// Roda o caminho real do S4.1 (fonte -> x264enc -> ring buffer em /dev/shm)
// por N segundos e mede, depois de um aquecimento:
//   * CPU do processo (getrusage): % de UM nucleo e % da CPU inteira;
//   * quadros por segundo que chegaram ao buffer e descartes;
//   * bitrate efetivo do que o buffer guarda e RAM da arena (/dev/shm) e RSS.
//
// Fontes:
//   fake     P4 falso em processo (NV12 + ods.p4.frame) — o caminho da 6c;
//   csi      camera CSI aberta pelo proprio S4 (nvarguscamerasrc, so na Jetson);
//   testsrc  videotestsrc (qualquer PC com GStreamer).
//
// Saida: texto legivel + uma linha JSON (evidencia minima do manual, item 18).
#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/infrastructure/gstreamer/software_h264_encoder.hpp"

#if defined(ODS_S4_WITH_GSTREAMER)
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/p4_raw_frame_encoder_source.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/ringbuffer/fake_p4_producer.hpp"
#endif

using namespace ods::s4;

#if defined(ODS_S4_WITH_GSTREAMER)

namespace {

struct Options {
    std::string source{"fake"};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    double fps{30.0};
    double seconds{20.0};
    double warmupSeconds{3.0};
    double windowSeconds{30.0};
    int sensorId{0};
    infrastructure::SoftwareH264EncoderSettings encoder;
};

void print_usage(const char* program) {
    std::cerr << "usage: " << program << " [--source fake|csi|testsrc] [--width W] [--height H]\n"
                 "       [--fps F] [--seconds N] [--warmup N] [--window-seconds N] [--sensor-id N]\n"
                 "       [--encoder-preset NAME] [--encoder-bitrate BPS] [--gop N] [--encoder-threads N]\n"
                 "\n"
                 "Padrao: --source fake 1920x1080@30, 20 s medidos apos 3 s de aquecimento,\n"
                 "x264enc ultrafast 4 Mbps GOP 30. Rode com a inferencia ligada para o numero valer.\n";
}

bool parse(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&](const char* flag) {
            return argument == flag && i + 1 < argc ? argv[++i] : nullptr;
        };
        if (const char* v = value("--source")) {
            options.source = v;
        } else if (const char* v = value("--width")) {
            options.width = static_cast<std::uint32_t>(std::stoul(v));
        } else if (const char* v = value("--height")) {
            options.height = static_cast<std::uint32_t>(std::stoul(v));
        } else if (const char* v = value("--fps")) {
            options.fps = std::stod(v);
        } else if (const char* v = value("--seconds")) {
            options.seconds = std::stod(v);
        } else if (const char* v = value("--warmup")) {
            options.warmupSeconds = std::stod(v);
        } else if (const char* v = value("--window-seconds")) {
            options.windowSeconds = std::stod(v);
        } else if (const char* v = value("--sensor-id")) {
            options.sensorId = std::stoi(v);
        } else if (const char* v = value("--encoder-preset")) {
            options.encoder.speedPreset = infrastructure::parse_x264_speed_preset(v);
        } else if (const char* v = value("--encoder-bitrate")) {
            options.encoder.bitrateBps = static_cast<std::size_t>(std::stoull(v));
        } else if (const char* v = value("--gop")) {
            options.encoder.keyframeInterval = std::stoi(v);
        } else if (const char* v = value("--encoder-threads")) {
            options.encoder.threads = std::stoi(v);
        } else {
            return false;
        }
    }
    return options.seconds > 0.0 && options.warmupSeconds >= 0.0 &&
           (options.source == "fake" || options.source == "csi" || options.source == "testsrc");
}

double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1e6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

long resident_kilobytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return std::stol(line.substr(6));
        }
    }
    return -1;
}

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    try {
        if (!parse(argc, argv, options)) {
            print_usage(argv[0]);
            return 1;
        }
        options.encoder.validate();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }

    const domain::RawVideoFormat format(options.width, options.height, options.fps);
    application::RingBufferConfig config;
    config.cameraId = "measure_" + std::to_string(::getpid());
    config.windowSeconds = options.windowSeconds;
    config.bitrateBps = options.encoder.bitrateBps;
    config.width = options.width;
    config.height = options.height;
    config.replaceStaleSegment = true;

    infrastructure::P4RawFrameEncoderSource* p4Source = nullptr;
    std::unique_ptr<domain::IFrameSource> source;
    if (options.source == "fake") {
        auto owned = std::make_unique<infrastructure::P4RawFrameEncoderSource>(
            config.cameraId, format, options.encoder);
        p4Source = owned.get();
        source = std::move(owned);
    } else if (options.source == "csi") {
        infrastructure::CsiCameraOptions camera;
        camera.sensorId = options.sensorId;
        camera.width = static_cast<int>(options.width);
        camera.height = static_cast<int>(options.height);
        camera.fps = static_cast<int>(options.fps);
        camera.encoder = options.encoder;
        source = std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
            infrastructure::GStreamerAppsinkFrameSource::jetsonCsiH264Pipeline(camera), "measure-csi");
    } else {
        source = std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
            "videotestsrc is-live=true pattern=ball ! " + infrastructure::raw_video_caps(format) +
                " ! " + infrastructure::raw_to_h264_chain(format, options.encoder) +
                " ! appsink name=ods_sink emit-signals=true sync=false max-buffers=8 drop=false",
            "measure-testsrc");
    }

    auto facade = std::make_shared<infrastructure::RingBufferRAMFacade>(config, std::move(source));
    std::unique_ptr<infrastructure::FakeP4Feed> feed;
    try {
        facade->startCapture("measure");
        if (p4Source != nullptr) {
            infrastructure::FakeP4Options fakeOptions;
            fakeOptions.sourceId = config.cameraId;
            fakeOptions.format = format;
            feed = std::make_unique<infrastructure::FakeP4Feed>(
                fakeOptions,
                [p4Source](const domain::P4FrameMetadata& metadata, const std::uint8_t* data,
                           std::size_t length) { p4Source->submit(metadata, data, length); },
                [facade](const domain::SessionStarted& marker) { facade->onSessionStarted(marker); });
            feed->start();
        }
    } catch (const std::exception& error) {
        std::cerr << "cannot start the capture: " << error.what() << '\n';
        return 1;
    }

    std::cout << "measuring " << options.source << " " << options.width << "x" << options.height
              << "@" << options.fps << " with " << options.encoder.gstElement() << '\n'
              << "warm-up " << options.warmupSeconds << " s, then " << options.seconds << " s...\n";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::duration<double>(options.warmupSeconds));

    const auto before = facade->stats();
    const std::uint64_t droppedBefore = p4Source != nullptr ? p4Source->framesDroppedTotal() : 0;
    const double cpuBefore = cpu_seconds();
    const auto wallBefore = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
    const double cpuUsed = cpu_seconds() - cpuBefore;
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wallBefore).count();
    const auto after = facade->stats();
    const std::uint64_t dropped =
        (p4Source != nullptr ? p4Source->framesDroppedTotal() : 0) - droppedBefore;
    const long rssKb = resident_kilobytes();

    if (feed) {
        feed->stop();
    }
    facade->stopCapture();

    const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
    const double cpuOneCorePercent = 100.0 * cpuUsed / wall;
    const double fpsIngested =
        static_cast<double>(after.framesIngestedTotal - before.framesIngestedTotal) / wall;
    const double storedBitrateBps =
        after.spanSeconds() > 0.0 ? 8.0 * static_cast<double>(after.bytesUsed) / after.spanSeconds() : 0.0;

    std::cout << std::fixed << std::setprecision(1)
              << "CPU:       " << cpuOneCorePercent << "% of one core ("
              << cpuOneCorePercent / cores << "% of " << cores << " cores)\n"
              << "frames:    " << fpsIngested << " fps into the buffer, " << dropped << " dropped, "
              << facade->framesRejectedTotal() << " rejected\n"
              << "bitrate:   " << storedBitrateBps / 1e6 << " Mbps stored (target "
              << static_cast<double>(options.encoder.bitrateBps) / 1e6 << ")\n"
              << "buffer:    " << static_cast<double>(after.bytesUsed) / 1048576.0 << " of "
              << static_cast<double>(after.capacityBytes) / 1048576.0 << " MB in /dev/shm, span "
              << after.spanSeconds() << " s\n"
              << "process:   " << static_cast<double>(rssKb) / 1024.0 << " MB resident\n";

    std::ostringstream json;
    json << std::fixed << std::setprecision(3) << "{\"source\":\"" << options.source
         << "\",\"width\":" << options.width << ",\"height\":" << options.height
         << ",\"fps\":" << options.fps << ",\"preset\":\""
         << infrastructure::to_string(options.encoder.speedPreset)
         << "\",\"bitrate_bps\":" << options.encoder.bitrateBps
         << ",\"gop\":" << options.encoder.keyframeInterval
         << ",\"threads\":" << options.encoder.threads << ",\"seconds\":" << wall
         << ",\"cpu_percent_one_core\":" << cpuOneCorePercent << ",\"cores\":" << cores
         << ",\"fps_ingested\":" << fpsIngested << ",\"frames_dropped\":" << dropped
         << ",\"frames_rejected\":" << facade->framesRejectedTotal()
         << ",\"stored_bitrate_bps\":" << storedBitrateBps
         << ",\"buffer_capacity_bytes\":" << after.capacityBytes
         << ",\"buffer_used_bytes\":" << after.bytesUsed << ",\"rss_kb\":" << rssKb << "}";
    std::cout << json.str() << '\n';
    return 0;
}

#else

int main() {
    std::cerr << "s4_measure_encoder needs GStreamer: rebuild with -DODS_S4_WITH_GSTREAMER=ON\n";
    return 1;
}

#endif
