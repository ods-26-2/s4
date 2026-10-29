// S4.1 — captura REAL via GStreamer: pipeline x264 -> appsink -> ring buffer.
//
// Prova o caminho que os outros testes nao exercitam (eles usam fontes
// sintetica e gravada): o adaptador GStreamerAppsinkFrameSource compila, entrega
// quadros com timestamp e flag de keyframe corretos, nao rejeita B-frames, e o
// trecho extraido do buffer e de fato DECODIFICAVEL por um decoder H.264.
//
// Sem GStreamer (ou sem os plugins x264enc/avdec_h264) o teste e pulado com o
// codigo 77, que o CTest reporta como "Skipped" em vez de "Passed".
#include "tests/ods_check.hpp"

#include <iostream>

#if !defined(ODS_S4_WITH_GSTREAMER)

int main() {
    std::cout << "test_s4_gstreamer_capture: SKIPPED (built without GStreamer)\n";
    return 77;
}

#else

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"

using namespace ods::s4;

namespace {

constexpr int kSkipped = 77;
constexpr int kFrameCount = 60;

bool has_elements(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory == nullptr) {
            std::cout << "test_s4_gstreamer_capture: SKIPPED (missing element " << name << ")\n";
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

// Sem h264parse (plugins-bad): o x264enc ja entrega byte-stream com um access
// unit por buffer, que e exatamente o contrato que o appsink do S4.1 espera.
std::string x264_pipeline(const std::string& encoderOptions) {
    return "videotestsrc num-buffers=" + std::to_string(kFrameCount) +
           " ! video/x-raw,width=320,height=240,framerate=30/1"
           " ! x264enc speed-preset=ultrafast key-int-max=15 " + encoderOptions +
           " ! video/x-h264,stream-format=byte-stream,alignment=au"
           " ! appsink name=ods_sink emit-signals=true sync=false max-buffers=8 drop=false";
}

std::unique_ptr<infrastructure::RingBufferRAMFacade> start_capture(const std::string& pipeline) {
    static int counter = 0;
    application::RingBufferConfig config;
    config.cameraId = "gst_test_" + std::to_string(++counter) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
    config.windowSeconds = 30.0;
    config.bitrateBps = 2000000;
    auto facade = std::make_unique<infrastructure::RingBufferRAMFacade>(
        config,
        std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(pipeline, "gst-session-1"));
    facade->startCapture(pipeline);
    return facade;
}

// Espera todos os quadros (aceitos ou rejeitados) chegarem ao buffer.
void wait_for_all_frames(const infrastructure::RingBufferRAMFacade& facade) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto handled = facade.stats().framesIngestedTotal + facade.framesRejectedTotal();
        if (handled >= static_cast<std::uint64_t>(kFrameCount)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Alimenta um decoder H.264 real com o trecho, um access unit por buffer, e
// conta quantas imagens saem. Um trecho que nao comeca em keyframe (ou que
// perdeu quadros de referencia) produz menos imagens ou erro no barramento.
std::size_t decoded_frame_count(const application::ExtractedSegment& segment, bool& hadError) {
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "appsrc name=src format=time caps=video/x-h264,stream-format=byte-stream,alignment=au"
        " ! avdec_h264 ! appsink name=out sync=false", &error);
    ODS_CHECK(pipeline != nullptr && error == nullptr);
    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement* out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    for (const auto& frame : segment.frames) {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.data.size(), nullptr);
        gst_buffer_fill(buffer, 0, frame.data.data(), frame.data.size());
        gst_app_src_push_buffer(GST_APP_SRC(src), buffer);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    std::size_t decoded = 0;
    while (GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(out))) {
        ++decoded;
        gst_sample_unref(sample);
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    hadError = message != nullptr;
    if (message != nullptr) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(out);
    gst_object_unref(pipeline);
    return decoded;
}

void check_capture_and_decodable_segment(const std::string& encoderOptions) {
    auto facade = start_capture(x264_pipeline(encoderOptions));
    wait_for_all_frames(*facade);
    const auto stats = facade->stats();

    ODS_CHECK(facade->framesRejectedTotal() == 0u);
    ODS_CHECK(stats.framesIngestedTotal == static_cast<std::uint64_t>(kFrameCount));
    ODS_CHECK(stats.sessionId == "gst-session-1");

    // Janela comecando no MEIO do stream: o buffer precisa recuar ate um
    // keyframe para que o trecho decodifique.
    const domain::Nanoseconds middle =
        stats.oldestCaptureTsNs + (stats.newestCaptureTsNs - stats.oldestCaptureTsNs) / 2 + 1;
    const auto segment = facade->extractCaptureWindow(
        domain::CaptureWindow(middle, stats.newestCaptureTsNs));
    facade->stopCapture();

    ODS_CHECK(!segment.frames.empty());
    ODS_CHECK(segment.frames.front().isKeyframe);
    ODS_CHECK(segment.firstCaptureTsNs() <= middle);

    bool hadError = false;
    const std::size_t decoded = decoded_frame_count(segment, hadError);
    ODS_CHECK(!hadError);
    ODS_CHECK(decoded == segment.frames.size());
}

} // namespace

void test_deve_ingerir_e_entregar_trecho_decodificavel_quando_camera_x264_nao_usar_b_frames() {
    check_capture_and_decodable_segment("tune=zerolatency");
}

// Replay de .mp4 comum tem B-frames: o PTS sai fora da ordem de decodificacao
// e, carimbado pelo PTS, cada B-frame seria rejeitado como relogio voltando.
void test_nao_deve_rejeitar_quadros_quando_stream_tiver_b_frames() {
    check_capture_and_decodable_segment("bframes=2 b-adapt=false");
}

void test_deve_lancar_excecao_quando_pipeline_nao_terminar_no_appsink_do_s4() {
    infrastructure::GStreamerAppsinkFrameSource source("videotestsrc ! fakesink", "s");
    bool threw = false;
    try {
        source.start([](const domain::CapturedFrame&) {});
    } catch (const domain::FrameSourceError&) {
        threw = true;
    }
    ODS_CHECK(threw);
    ODS_CHECK(!source.isRunning());
}

int main() {
    gst_init(nullptr, nullptr);
    if (!has_elements({"videotestsrc", "x264enc", "appsink", "appsrc", "avdec_h264"})) {
        return kSkipped;
    }

    test_deve_ingerir_e_entregar_trecho_decodificavel_quando_camera_x264_nao_usar_b_frames();
    test_nao_deve_rejeitar_quadros_quando_stream_tiver_b_frames();
    test_deve_lancar_excecao_quando_pipeline_nao_terminar_no_appsink_do_s4();

    std::cout << "test_s4_gstreamer_capture: all tests passed\n";
    return 0;
}

#endif
