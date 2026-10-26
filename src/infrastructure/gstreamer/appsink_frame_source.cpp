#include "s4/infrastructure/gstreamer/appsink_frame_source.hpp"

#include <cstdio>
#include <optional>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

// Um access unit por buffer, em Annex-B: concatenar quadros produz um .h264
// valido e nenhum buffer traz "meio quadro".
constexpr const char* kH264Caps = "video/x-h264,stream-format=byte-stream,alignment=au";

// sync=false porque isto nao e um player (nao esperar o relogio de exibicao);
// drop=false porque descartar quadro em silencio produziria um clipe com
// buraco invisivel — preferimos backpressure, que aparece nas metricas.
constexpr const char* kAppsinkTail =
    "appsink name=ods_sink emit-signals=true sync=false max-buffers=8 drop=false";

} // namespace

namespace {

// Aspas em torno de um caminho dentro de uma descricao de pipeline: sem elas
// um nome de arquivo com espaco ("10 Minutes of Amazon Rainforest.mp4") corta
// o parse no meio e sobra um pipeline incompleto.
std::string quoted(const std::string& value) {
    return "\"" + value + "\"";
}

} // namespace

std::string GStreamerAppsinkFrameSource::jetsonCsiH264Pipeline(const CsiCameraOptions& o) {
    // nvvidconv copia o quadro da memoria NVMM (ISP) para memoria de sistema,
    // que e onde o x264enc le. O nvv4l2h264enc que estava aqui depende do NVENC,
    // que a Orin Nano nao tem.
    const domain::RawVideoFormat format(
        static_cast<std::uint32_t>(o.width), static_cast<std::uint32_t>(o.height), o.fps
    );
    return "nvarguscamerasrc sensor-id=" + std::to_string(o.sensorId) +
           " ! video/x-raw(memory:NVMM),width=" + std::to_string(o.width) +
           ",height=" + std::to_string(o.height) + ",framerate=" + std::to_string(o.fps) +
           "/1 ! nvvidconv ! " + raw_video_caps(format) + " ! " +
           // h264parse config-interval=-1 repete SPS/PPS em TODO keyframe; sem
           // isso, um trecho que comeca no meio do stream nao decodifica.
           raw_to_h264_chain(format, o.encoder) + " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::shmH264Pipeline(const std::string& socketPath) {
    return "shmsrc socket-path=" + quoted(socketPath) +
           " is-live=true do-timestamp=true ! h264parse config-interval=-1 ! " + kH264Caps +
           " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::p4RawShmPipeline(
    const std::string& socketPath,
    const domain::RawVideoFormat& format,
    const SoftwareH264EncoderSettings& encoder
) {
    // shmsrc nao anuncia caps: o filtro logo depois diz ao x264enc a geometria
    // combinada com P4. Cada buffer do shmsink e um quadro inteiro.
    return "shmsrc socket-path=" + quoted(socketPath) +
           " is-live=true do-timestamp=true ! " + raw_video_caps(format) + " ! " +
           raw_to_h264_chain(format, encoder) + " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::fileReplayH264Pipeline(const std::string& path) {
    return "filesrc location=" + quoted(path) + " ! qtdemux ! h264parse config-interval=-1 ! " +
           kH264Caps + " ! " + kAppsinkTail;
}

std::string GStreamerAppsinkFrameSource::testPatternH264Pipeline(
    int width, int height, int fps, int bitrateKbps, int gop
) {
    // openh264enc e o encoder H.264 que acompanha o plugin gstreamer1-plugin-
    // openh264, ao contrario do x264enc (que vive no pacote bad/good e nao vem
    // em varias imagens). A propriedade do intervalo de keyframes chama-se
    // gop-size; sem ela o stream sairia com um unico IDR e o S4.1 nunca
    // encontraria um keyframe para alinhar o inicio do trecho.
    // Na Jetson este caminho nao e o de producao: na 6c os quadros vem crus de
    // P4 e sao codificados pelo x264enc (P4RawFrameEncoderSource ou
    // p4RawShmPipeline()).
    return "videotestsrc is-live=true ! video/x-raw,width=" + std::to_string(width) +
           ",height=" + std::to_string(height) + ",framerate=" + std::to_string(fps) +
           "/1 ! openh264enc bitrate=" + std::to_string(bitrateKbps * 1000) +
           " gop-size=" + std::to_string(gop) + " ! h264parse config-interval=-1 ! " +
           kH264Caps + " ! " + kAppsinkTail;
}

#if defined(ODS_S4_WITH_GSTREAMER)

} // namespace ods::s4::infrastructure

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "s4/infrastructure/gstreamer/gst_capture_timestamp.hpp"

namespace ods::s4::infrastructure {

struct GStreamerAppsinkFrameSource::Impl {
    GstElement* pipeline{nullptr};
    GstElement* sink{nullptr};
    GMainLoop* loop{nullptr};
    GThread* loopThread{nullptr};
    domain::FrameCallback onFrame;
    std::string sessionId;
};

// Sem o meta, capture_ts = base_time do pipeline + timestamp do buffer
// (relogio monotonico do GStreamer).
//
// Usa o DTS quando ele existe: o buffer guarda os quadros em ORDEM DE
// DECODIFICACAO, que e a ordem em que o appsink os entrega e a unica em que a
// concatenacao forma um H.264 valido. Com B-frames (replay de .mp4 comum, e o
// caso do video de video_test/) o PTS e a ordem de APRESENTACAO e chega fora de
// ordem -- 0, 100 ms, 33 ms, 133 ms... -- e o RingBuffer exige ordem de CHEGADA,
// descartando qualquer quadro com captureTs menor que o ultimo: 2 de cada 3
// quadros desse video seriam rejeitados. O DTS e monotonico na ordem de
// decodificacao, que e exatamente a ordem em que os access units saem do
// demuxer. Sem B-frames, que e o caso do x264enc do S4.1, DTS e PTS coincidem e
// o valor continua sendo o instante de captura; fonte sem DTS cai no PTS.
//
// Quadro sem DTS E sem PTS e descartado: nao ha onde posiciona-lo na linha do
// tempo, e carimba-lo com o base_time do pipeline o colocaria no comeco da
// gravacao -- o RingBuffer o rejeitaria como relogio retrocedendo e a perda
// seria silenciosa.
//
// O meta "timestamp/x-unix" e a base de tempo oficial de B2 (captured_at): e a
// troca que este comentario anunciava quando TRA-1/TRA-2 fechassem a base.
std::optional<CaptureInstant> capture_instant_of(GstClockTime pipelineBaseTime, GstBuffer* buffer) {
    static GstCaps* const unixReference = gst_caps_new_empty_simple(kUnixTimestampReference);
    const GstReferenceTimestampMeta* reference =
        gst_buffer_get_reference_timestamp_meta(buffer, unixReference);
    if (reference != nullptr && GST_CLOCK_TIME_IS_VALID(reference->timestamp)) {
        return CaptureInstant{
            static_cast<domain::Nanoseconds>(reference->timestamp),
            domain::CaptureTimeBase::CapturedAtUtc
        };
    }
    const GstClockTime bufferTime =
        GST_BUFFER_DTS_IS_VALID(buffer) ? GST_BUFFER_DTS(buffer) : GST_BUFFER_PTS(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(bufferTime)) {
        return std::nullopt;
    }
    return CaptureInstant{
        static_cast<domain::Nanoseconds>(pipelineBaseTime) +
            static_cast<domain::Nanoseconds>(bufferTime),
        domain::CaptureTimeBase::SourceClock
    };
}

namespace {

GstFlowReturn on_new_sample(GstAppSink* sink, gpointer userData) {
    auto* impl = static_cast<GStreamerAppsinkFrameSource::Impl*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    const std::optional<CaptureInstant> instant =
        capture_instant_of(gst_element_get_base_time(impl->pipeline), buffer);
    if (!instant.has_value()) {
        // Sem DTS nem PTS nao ha como posicionar o quadro na linha do tempo.
        std::fprintf(stderr, "[S4.1] frame without timestamp dropped\n");
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    domain::CapturedFrame frame;
    frame.captureTsNs = instant->captureTsNs;
    frame.timeBase = instant->timeBase;
    frame.data = static_cast<const std::uint8_t*>(info.data);
    frame.length = info.size;
    // Sem a flag DELTA_UNIT o buffer e um keyframe.
    frame.isKeyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    frame.sessionId = impl->sessionId;
    if (impl->onFrame) {
        impl->onFrame(frame);
    }

    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

} // namespace

GStreamerAppsinkFrameSource::GStreamerAppsinkFrameSource(
    std::string pipelineDescription,
    std::string sessionId
) : m_impl(new Impl()),
    m_pipelineDescription(std::move(pipelineDescription)),
    m_sessionId(std::move(sessionId)) {
    m_impl->sessionId = m_sessionId;
}

GStreamerAppsinkFrameSource::~GStreamerAppsinkFrameSource() {
    stop();
    delete m_impl;
}

void GStreamerAppsinkFrameSource::start(domain::FrameCallback onFrame) {
    if (isRunning()) {
        return;
    }
    gst_init(nullptr, nullptr);
    m_impl->onFrame = std::move(onFrame);

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(m_pipelineDescription.c_str(), &error);
    // gst_parse_launch devolve um pipeline parcial e um GError quando encontra
    // erro no meio da descricao. Aceitar esse parcial adia a falha para um
    // sintoma enganoso (ex: "pipeline must end in appsink"), entao qualquer
    // GError aqui ja e falha definitiva.
    if (m_impl->pipeline == nullptr || error != nullptr) {
        const std::string message = error != nullptr ? error->message : "unknown";
        if (m_impl->pipeline != nullptr) {
            gst_object_unref(m_impl->pipeline);
            m_impl->pipeline = nullptr;
        }
        if (error != nullptr) {
            g_error_free(error);
        }
        throw domain::FrameSourceError(
            "invalid pipeline [" + m_pipelineDescription + "]: " + message
        );
    }

    m_impl->sink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "ods_sink");
    if (m_impl->sink == nullptr) {
        stop();
        throw domain::FrameSourceError("pipeline must end in 'appsink name=ods_sink'");
    }
    g_signal_connect(m_impl->sink, "new-sample", G_CALLBACK(on_new_sample), m_impl);

    m_impl->loop = g_main_loop_new(nullptr, FALSE);
    m_impl->loopThread = g_thread_new(
        "ods-gst-loop",
        [](gpointer data) -> gpointer {
            g_main_loop_run(static_cast<GMainLoop*>(data));
            return nullptr;
        },
        m_impl->loop
    );

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stop();
        throw domain::FrameSourceError("pipeline could not reach PLAYING");
    }
}

void GStreamerAppsinkFrameSource::stop() {
    if (m_impl->pipeline != nullptr) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        if (m_impl->sink != nullptr) {
            // gst_bin_get_by_name devolveu uma referencia propria.
            gst_object_unref(m_impl->sink);
            m_impl->sink = nullptr;
        }
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }
    if (m_impl->loop != nullptr) {
        g_main_loop_quit(m_impl->loop);
        if (m_impl->loopThread != nullptr) {
            g_thread_join(m_impl->loopThread);
            m_impl->loopThread = nullptr;
        }
        g_main_loop_unref(m_impl->loop);
        m_impl->loop = nullptr;
    }
}

bool GStreamerAppsinkFrameSource::isRunning() const {
    return m_impl != nullptr && m_impl->pipeline != nullptr;
}

#else // sem GStreamer: os pipelines acima continuam disponiveis como texto

struct GStreamerAppsinkFrameSource::Impl {};

GStreamerAppsinkFrameSource::GStreamerAppsinkFrameSource(
    std::string pipelineDescription,
    std::string sessionId
) : m_impl(nullptr),
    m_pipelineDescription(std::move(pipelineDescription)),
    m_sessionId(std::move(sessionId)) {}

GStreamerAppsinkFrameSource::~GStreamerAppsinkFrameSource() = default;

void GStreamerAppsinkFrameSource::start(domain::FrameCallback) {
    throw domain::FrameSourceError(
        "built without GStreamer: rebuild with -DODS_S4_WITH_GSTREAMER=ON"
    );
}

void GStreamerAppsinkFrameSource::stop() {}

bool GStreamerAppsinkFrameSource::isRunning() const { return false; }

#endif

} // namespace ods::s4::infrastructure
