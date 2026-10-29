// S4.1 na arquitetura 6c — entrada de P4 com encoder por software.
//
// Prova, com GStreamer e x264enc de verdade:
//   * quadros crus (NV12) de P4 + ods.p4.frame entram, H.264 sai, e o indice
//     usa o captured_at OFICIAL (UTC) e o session_id de P4 (itens 2, 3 e 4);
//   * o trecho extraido decodifica no avdec_h264 (item 1: x264enc sem B-frames);
//   * LOOP: o buffer zera, o relogio voltando nao e rejeitado e a sessao nova
//     comeca em keyframe (force-key-unit);
//   * o GstReferenceTimestampMeta "timestamp/x-unix" vira captured_at oficial;
//   * o caminho shmsrc com NV12 cru (P4 via shmsink) codifica e herda a sessao.
//
// Sem GStreamer, x264enc ou avdec_h264 o teste sai com 77 (Skipped).
#include "tests/ods_check.hpp"

#include <iostream>

#if !defined(ODS_S4_WITH_GSTREAMER)

int main() {
    std::cout << "test_s4_p4_input: SKIPPED (built without GStreamer)\n";
    return 77;
}

#else

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "s4/application/ring_buffer_config.hpp"
#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"
#include "s4/infrastructure/gstreamer/gst_capture_timestamp.hpp"
#include "s4/infrastructure/gstreamer/p4_raw_frame_encoder_source.hpp"
#include "s4/infrastructure/gstreamer/ring_buffer_ram_facade.hpp"
#include "s4/infrastructure/ringbuffer/fake_p4_producer.hpp"

using namespace ods::s4;
using domain::kNanosecondsPerSecond;
using domain::Nanoseconds;

namespace {

constexpr int kSkipped = 77;
constexpr Nanoseconds S = kNanosecondsPerSecond;
// 2026-09-23T14:29:47Z: o inicio do clipe do exemplo da 6c.
constexpr Nanoseconds kClipStartUtc = 1790173787LL * S;

const domain::RawVideoFormat kFormat(320, 240, 30.0);

bool has_elements(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory == nullptr) {
            std::cout << "test_s4_p4_input: SKIPPED (missing element " << name << ")\n";
            return false;
        }
        gst_object_unref(factory);
    }
    return true;
}

std::string unique_camera(const std::string& prefix) {
    static int counter = 0;
    return prefix + std::to_string(++counter) + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
}

application::RingBufferConfig config_for(const std::string& cameraId) {
    application::RingBufferConfig config;
    config.cameraId = cameraId;
    config.windowSeconds = 30.0;
    config.bitrateBps = 1000000;
    config.width = kFormat.width();
    config.height = kFormat.height();
    return config;
}

infrastructure::SoftwareH264EncoderSettings test_encoder() {
    infrastructure::SoftwareH264EncoderSettings encoder;
    encoder.speedPreset = infrastructure::X264SpeedPreset::Ultrafast;
    encoder.bitrateBps = 1000000;
    return encoder;
}

// Facade + fonte de P4; o ponteiro cru e para o teste chamar submit().
struct P4Fixture {
    std::string cameraId{unique_camera("p4_")};
    infrastructure::P4RawFrameEncoderSource* source{nullptr};
    std::unique_ptr<infrastructure::RingBufferRAMFacade> facade;

    P4Fixture() {
        auto owned = std::make_unique<infrastructure::P4RawFrameEncoderSource>(
            cameraId, kFormat, test_encoder());
        source = owned.get();
        facade = std::make_unique<infrastructure::RingBufferRAMFacade>(
            config_for(cameraId), std::move(owned));
        facade->startCapture("p4");
    }

    ~P4Fixture() { facade->stopCapture(); }

    infrastructure::FakeP4Producer producer() const {
        infrastructure::FakeP4Options options;
        options.sourceId = cameraId;
        options.format = kFormat;
        options.startCapturedAtUtcNs = kClipStartUtc;
        options.distinctImages = 10;
        return infrastructure::FakeP4Producer(options);
    }

    void submit(infrastructure::FakeP4Producer& producer, int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            const auto next = producer.nextFrame();
            source->submit(next.metadata, next.pixels->data(), next.pixels->size());
            // O ritmo da camera nao importa aqui, mas a fila do appsrc (4
            // quadros) sim: o proximo quadro so entra quando o encoder ja
            // entregou quase todos os anteriores. Assim o teste vale para
            // qualquer velocidade de maquina (inclusive sob sanitizers), e um
            // descarte por atraso do encoder continua sendo falha.
            constexpr std::uint64_t kInFlight = 2;
            const std::uint64_t submitted = source->framesSubmittedTotal();
            if (submitted > kInFlight) {
                wait_for(submitted - kInFlight);
            }
        }
    }

    // Espera todos os quadros entregues (aceitos ou rejeitados) chegarem.
    void wait_for(std::uint64_t handled) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto stats = facade->stats();
            if (stats.framesIngestedTotal + facade->framesRejectedTotal() +
                    source->framesDroppedTotal() >= handled) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

// Decodifica o trecho no avdec_h264 e conta as imagens que saem.
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

// ==================================================== quadros crus de P4

void test_deve_indexar_pelo_captured_at_e_sessao_de_p4_quando_quadros_vierem_com_metadados() {
    P4Fixture fixture;
    auto producer = fixture.producer();
    fixture.facade->onSessionStarted(producer.currentSession());

    fixture.submit(producer, 60);
    fixture.wait_for(60);
    const auto stats = fixture.facade->stats();

    ODS_CHECK(fixture.facade->framesRejectedTotal() == 0u);
    ODS_CHECK(fixture.source->framesDroppedTotal() == 0u);
    ODS_CHECK(stats.framesIngestedTotal == 60u);
    ODS_CHECK(stats.sessionId == producer.currentSession().sessionId);
    // O indice esta no captured_at de P4, nao no relogio do GStreamer.
    ODS_CHECK(stats.oldestCaptureTsNs == kClipStartUtc);
    ODS_CHECK(stats.newestCaptureTsNs == kClipStartUtc + 59 * kFormat.frameDurationNs());
    ODS_CHECK(fixture.facade->timeBase() == domain::CaptureTimeBase::CapturedAtUtc);
    const auto wall = fixture.facade->toWallClock(kClipStartUtc);
    ODS_CHECK(std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch()).count() ==
              kClipStartUtc);
}

void test_deve_entregar_trecho_decodificavel_quando_s4_codificar_os_quadros_crus() {
    P4Fixture fixture;
    auto producer = fixture.producer();
    fixture.submit(producer, 75);
    fixture.wait_for(75);

    // Janela comecando no meio de um GOP (1 s = 30 quadros): recua ao keyframe.
    const Nanoseconds start = kClipStartUtc + S + S / 3;
    const auto segment = fixture.facade->extractCaptureWindow(
        domain::CaptureWindow(start, kClipStartUtc + 2 * S));

    ODS_CHECK(!segment.frames.empty());
    ODS_CHECK(segment.frames.front().isKeyframe);
    // Recua no maximo um GOP (30 quadros = 1 s) ate o keyframe anterior.
    ODS_CHECK(segment.firstCaptureTsNs() <= start);
    ODS_CHECK(segment.firstCaptureTsNs() > start - S);
    bool hadError = false;
    ODS_CHECK(decoded_frame_count(segment, hadError) == segment.frames.size());
    ODS_CHECK(!hadError);
}

void test_deve_zerar_buffer_e_comecar_em_keyframe_quando_p4_reiniciar_a_sessao_em_loop() {
    P4Fixture fixture;
    auto producer = fixture.producer();
    fixture.facade->onSessionStarted(producer.currentSession());
    fixture.submit(producer, 45);
    fixture.wait_for(45);

    // LOOP no meio de um GOP (quadro 45 de um GOP de 30): sem o keyframe
    // forcado, a sessao nova so seria decodificavel a partir do quadro 60.
    const auto loop = producer.startNewSession(domain::SessionChangeReason::Loop);
    ODS_CHECK(fixture.facade->onSessionStarted(loop));
    fixture.submit(producer, 20);
    fixture.wait_for(65);

    const auto stats = fixture.facade->stats();
    ODS_CHECK(fixture.facade->framesRejectedTotal() == 0u);
    ODS_CHECK(stats.sessionId == loop.sessionId);
    ODS_CHECK(stats.framesStored == 20u);
    // O captured_at voltou ao inicio da gravacao.
    ODS_CHECK(stats.oldestCaptureTsNs == kClipStartUtc);

    const auto segment = fixture.facade->extractCaptureWindow(
        domain::CaptureWindow(kClipStartUtc, kClipStartUtc + 19 * kFormat.frameDurationNs()));
    ODS_CHECK(segment.frames.size() == 20u);
    ODS_CHECK(segment.frames.front().isKeyframe);
    ODS_CHECK(!segment.isTruncatedAtStart);
    bool hadError = false;
    ODS_CHECK(decoded_frame_count(segment, hadError) == 20u);
    ODS_CHECK(!hadError);
}

void test_deve_recusar_quadro_quando_nao_bater_com_o_formato_combinado_com_p4() {
    P4Fixture fixture;
    auto producer = fixture.producer();
    const auto frame = producer.nextFrame();

    auto otherCamera = frame.metadata;
    otherCamera.sourceId = "cam-outra";
    ODS_CHECK(throws<domain::InvalidRawFrameError>([&] {
        fixture.source->submit(otherCamera, frame.pixels->data(), frame.pixels->size());
    }));

    auto otherSize = frame.metadata;
    otherSize.imageWidth = 640;
    ODS_CHECK(throws<domain::InvalidRawFrameError>([&] {
        fixture.source->submit(otherSize, frame.pixels->data(), frame.pixels->size());
    }));

    ODS_CHECK(throws<domain::InvalidRawFrameError>([&] {
        fixture.source->submit(frame.metadata, frame.pixels->data(), frame.pixels->size() - 1);
    }));

    auto noSession = frame.metadata;
    noSession.sessionId.clear();
    ODS_CHECK(throws<domain::InvalidRawFrameError>([&] {
        fixture.source->submit(noSession, frame.pixels->data(), frame.pixels->size());
    }));
}

void test_deve_lancar_excecao_quando_quadro_chegar_com_a_fonte_parada() {
    infrastructure::P4RawFrameEncoderSource source("cam-parada", kFormat, test_encoder());
    infrastructure::FakeP4Options options;
    options.sourceId = "cam-parada";
    options.format = kFormat;
    options.distinctImages = 1;
    infrastructure::FakeP4Producer producer(options);
    const auto frame = producer.nextFrame();

    ODS_CHECK(throws<domain::FrameSourceError>([&] {
        source.submit(frame.metadata, frame.pixels->data(), frame.pixels->size());
    }));
}

// ============================ carimbo oficial via GstReferenceTimestampMeta

void test_deve_usar_captured_at_oficial_quando_buffer_tiver_meta_timestamp_x_unix() {
    GstBuffer* buffer = gst_buffer_new();
    GST_BUFFER_PTS(buffer) = 5 * GST_SECOND;
    GstCaps* reference = gst_caps_new_empty_simple(infrastructure::kUnixTimestampReference);
    gst_buffer_add_reference_timestamp_meta(
        buffer, reference, static_cast<GstClockTime>(kClipStartUtc), GST_CLOCK_TIME_NONE);
    gst_caps_unref(reference);

    const auto instant = infrastructure::capture_instant_of(100 * GST_SECOND, buffer);

    ODS_CHECK(instant.has_value());
    ODS_CHECK(instant->captureTsNs == kClipStartUtc);
    ODS_CHECK(instant->timeBase == domain::CaptureTimeBase::CapturedAtUtc);
    gst_buffer_unref(buffer);
}

void test_deve_usar_relogio_do_pipeline_quando_buffer_nao_tiver_meta() {
    GstBuffer* buffer = gst_buffer_new();
    GST_BUFFER_PTS(buffer) = 7 * GST_SECOND;
    GST_BUFFER_DTS(buffer) = 6 * GST_SECOND;

    const auto instant = infrastructure::capture_instant_of(100 * GST_SECOND, buffer);
    GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    const auto none = infrastructure::capture_instant_of(100 * GST_SECOND, buffer);

    ODS_CHECK(instant.has_value());
    ODS_CHECK(instant->captureTsNs == 106 * S);
    ODS_CHECK(instant->timeBase == domain::CaptureTimeBase::SourceClock);
    ODS_CHECK(!none.has_value());
    gst_buffer_unref(buffer);
}

// ================================= P4 via shmsink com NV12 cru (sem metadados)

void test_deve_codificar_nv12_cru_recebido_por_shm_e_herdar_a_sessao_anunciada() {
    const std::string socketPath =
        (std::filesystem::temp_directory_path() / unique_camera("ods_p4_shm_")).string();
    // P4 simulado: NV12 cru publicado por shmsink, como o SIS-2 faria.
    GError* error = nullptr;
    GstElement* producer = gst_parse_launch(
        ("videotestsrc is-live=true pattern=ball ! " + infrastructure::raw_video_caps(kFormat) +
         " ! shmsink socket-path=" + socketPath +
         " shm-size=4000000 wait-for-connection=false sync=false").c_str(),
        &error);
    ODS_CHECK(producer != nullptr && error == nullptr);
    gst_element_set_state(producer, GST_STATE_PLAYING);
    for (int attempt = 0; attempt < 100 && !std::filesystem::exists(socketPath); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ODS_CHECK(std::filesystem::exists(socketPath));

    const std::string cameraId = unique_camera("p4shm_");
    // Sessao vazia: o shm nao carrega o ods.p4.frame, a sessao vem do marcador.
    auto facade = std::make_unique<infrastructure::RingBufferRAMFacade>(
        config_for(cameraId),
        std::make_unique<infrastructure::GStreamerAppsinkFrameSource>(
            infrastructure::GStreamerAppsinkFrameSource::p4RawShmPipeline(
                socketPath, kFormat, test_encoder()),
            ""));
    domain::SessionStarted marker;
    marker.sessionId = "sessao-p4-shm";
    marker.sources = {cameraId};
    marker.reason = domain::SessionChangeReason::Start;
    facade->onSessionStarted(marker);
    facade->startCapture("shm");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline && facade->stats().spanSeconds() < 1.5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto stats = facade->stats();
    ODS_CHECK(stats.spanSeconds() >= 1.5);
    ODS_CHECK(stats.sessionId == "sessao-p4-shm");
    ODS_CHECK(facade->framesRejectedTotal() == 0u);

    const auto segment = facade->extractCaptureWindow(
        domain::CaptureWindow(stats.newestCaptureTsNs - S, stats.newestCaptureTsNs));
    facade->stopCapture();
    gst_element_set_state(producer, GST_STATE_NULL);
    gst_object_unref(producer);

    ODS_CHECK(segment.frames.front().isKeyframe);
    bool hadError = false;
    ODS_CHECK(decoded_frame_count(segment, hadError) == segment.frames.size());
    ODS_CHECK(!hadError);
}

int main() {
    gst_init(nullptr, nullptr);
    if (!has_elements({"appsrc", "appsink", "x264enc", "h264parse", "avdec_h264",
                       "videotestsrc", "shmsink", "shmsrc"})) {
        return kSkipped;
    }

    test_deve_indexar_pelo_captured_at_e_sessao_de_p4_quando_quadros_vierem_com_metadados();
    test_deve_entregar_trecho_decodificavel_quando_s4_codificar_os_quadros_crus();
    test_deve_zerar_buffer_e_comecar_em_keyframe_quando_p4_reiniciar_a_sessao_em_loop();
    test_deve_recusar_quadro_quando_nao_bater_com_o_formato_combinado_com_p4();
    test_deve_lancar_excecao_quando_quadro_chegar_com_a_fonte_parada();
    test_deve_usar_captured_at_oficial_quando_buffer_tiver_meta_timestamp_x_unix();
    test_deve_usar_relogio_do_pipeline_quando_buffer_nao_tiver_meta();
    test_deve_codificar_nv12_cru_recebido_por_shm_e_herdar_a_sessao_anunciada();

    std::cout << "test_s4_p4_input: all tests passed\n";
    return 0;
}

#endif
