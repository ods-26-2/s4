#include "s4/infrastructure/gstreamer/p4_raw_frame_encoder_source.hpp"

#include <cstdio>
#include <utility>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

// Mesmo final de pipeline do GStreamerAppsinkFrameSource: sync=false porque
// nao e player, drop=false porque descarte silencioso de quadro codificado
// abriria um buraco invisivel no clipe.
constexpr const char* kAppsinkTail = "appsink name=ods_sink sync=false max-buffers=8 drop=false";

std::string describe(const domain::P4FrameMetadata& metadata) {
    return std::to_string(metadata.imageWidth) + "x" + std::to_string(metadata.imageHeight) + " " +
           domain::to_string(metadata.pixelFormat);
}

std::string describe(const domain::RawVideoFormat& format) {
    return std::to_string(format.width()) + "x" + std::to_string(format.height()) + " " +
           domain::to_string(format.pixelFormat());
}

} // namespace

void P4RawFrameEncoderSource::rejectIfInvalid(
    const domain::P4FrameMetadata& metadata,
    std::size_t length
) const {
    if (metadata.sourceId != m_sourceId) {
        throw domain::InvalidRawFrameError(
            "frame from source '" + metadata.sourceId + "' sent to the buffer of '" + m_sourceId + "'"
        );
    }
    if (metadata.sessionId.empty()) {
        throw domain::InvalidRawFrameError("ods.p4.frame without session_id");
    }
    if (!metadata.matches(m_format)) {
        throw domain::InvalidRawFrameError(
            "frame is " + describe(metadata) + " but the negotiated format is " + describe(m_format)
        );
    }
    if (length != m_format.frameBytes()) {
        throw domain::InvalidRawFrameError(
            "frame has " + std::to_string(length) + " bytes; " + describe(m_format) + " needs " +
            std::to_string(m_format.frameBytes())
        );
    }
}

} // namespace ods::s4::infrastructure

#if defined(ODS_S4_WITH_GSTREAMER)

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace ods::s4::infrastructure {

struct P4RawFrameEncoderSource::Impl {
    struct Pending {
        GstClockTime runningTime;
        domain::P4FrameMetadata metadata;
    };

    // Guarda o ciclo de vida: submit() e stop() nunca mexem no pipeline ao
    // mesmo tempo.
    std::mutex lifecycleMutex;
    GstElement* pipeline{nullptr};
    GstElement* appsrc{nullptr};
    GstElement* appsink{nullptr};
    domain::FrameCallback onFrame;

    // Metadados a espera do access unit correspondente.
    std::mutex pendingMutex;
    std::deque<Pending> pending;
    GstClockTime nextRunningTime{0};
    std::string lastSessionId;

    std::mutex errorMutex;
    std::string pipelineError;

    GstClockTime frameDuration{0};
    guint64 maxQueuedBytes{0};
    std::atomic<std::uint64_t> submittedTotal{0};
    std::atomic<std::uint64_t> droppedTotal{0};

    // Running time de saida -> metadados do quadro de entrada. Entradas mais
    // antigas que o quadro que saiu foram descartadas pelo encoder.
    std::optional<domain::P4FrameMetadata> takeMetadataFor(GstClockTime runningTime) {
        std::lock_guard<std::mutex> lock(pendingMutex);
        while (!pending.empty() && pending.front().runningTime < runningTime) {
            pending.pop_front();
            droppedTotal.fetch_add(1);
        }
        if (pending.empty() || pending.front().runningTime != runningTime) {
            return std::nullopt;
        }
        domain::P4FrameMetadata metadata = std::move(pending.front().metadata);
        pending.pop_front();
        return metadata;
    }

    void recordError(const std::string& message) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (pipelineError.empty()) {
            pipelineError = message;
            std::fprintf(stderr, "[S4.1] P4 encoder pipeline error: %s\n", message.c_str());
        }
    }

    [[nodiscard]] std::string error() {
        std::lock_guard<std::mutex> lock(errorMutex);
        return pipelineError;
    }
};

namespace {

GstFlowReturn on_encoded_sample(GstAppSink* sink, gpointer userData) {
    auto* impl = static_cast<P4RawFrameEncoderSource::Impl*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    const GstSegment* segment = gst_sample_get_segment(sample);
    const GstClockTime runningTime =
        gst_segment_to_running_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    const std::optional<domain::P4FrameMetadata> metadata = impl->takeMetadataFor(runningTime);
    if (!metadata.has_value()) {
        // Sem metadados nao ha captured_at nem sessao: guardar o quadro com um
        // tempo inventado seria pior que perde-lo, e a perda fica registrada.
        impl->droppedTotal.fetch_add(1);
        std::fprintf(stderr, "[S4.1] encoded frame without ods.p4.frame metadata dropped\n");
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    domain::CapturedFrame frame;
    frame.captureTsNs = metadata->capturedAtUtcNs;
    frame.timeBase = domain::CaptureTimeBase::CapturedAtUtc;
    frame.data = static_cast<const std::uint8_t*>(info.data);
    frame.length = info.size;
    frame.isKeyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
    frame.sessionId = metadata->sessionId;
    if (impl->onFrame) {
        impl->onFrame(frame);
    }

    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Sem GMainLoop: o handler sincrono guarda o primeiro erro e descarta todas as
// mensagens, para que o barramento nao acumule mensagens de um pipeline que
// roda por dias.
GstBusSyncReply on_bus_message(GstBus*, GstMessage* message, gpointer userData) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        static_cast<P4RawFrameEncoderSource::Impl*>(userData)->recordError(
            error != nullptr ? error->message : "unknown"
        );
        if (error != nullptr) {
            g_error_free(error);
        }
        g_free(debug);
    }
    return GST_BUS_DROP;
}

// Pede ao x264enc um keyframe no quadro de running time "runningTime". O
// running time explicito faz o pedido valer para o quadro certo mesmo que o
// evento ultrapasse buffers ja enfileirados no appsrc.
void request_keyframe_at(GstElement* appsrc, GstClockTime runningTime) {
    GstStructure* structure = gst_structure_new(
        "GstForceKeyUnit",
        "running-time", G_TYPE_UINT64, static_cast<guint64>(runningTime),
        "all-headers", G_TYPE_BOOLEAN, TRUE,
        "count", G_TYPE_UINT, 0U,
        nullptr
    );
    gst_element_send_event(appsrc, gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM, structure));
}

} // namespace

P4RawFrameEncoderSource::P4RawFrameEncoderSource(
    std::string sourceId,
    domain::RawVideoFormat format,
    SoftwareH264EncoderSettings encoder
) : m_sourceId(std::move(sourceId)),
    m_format(format),
    m_encoder(encoder),
    m_impl(new Impl()) {
    m_pipelineDescription =
        "appsrc name=ods_src is-live=true format=time do-timestamp=false block=false max-bytes=0 caps=" +
        raw_video_caps(m_format) + " ! " + raw_to_h264_chain(m_format, m_encoder) + " ! " +
        kAppsinkTail;
    m_impl->frameDuration = static_cast<GstClockTime>(m_format.frameDurationNs());
    m_impl->maxQueuedBytes = static_cast<guint64>(kMaxQueuedFrames * m_format.frameBytes());
}

P4RawFrameEncoderSource::~P4RawFrameEncoderSource() {
    stop();
    delete m_impl;
}

void P4RawFrameEncoderSource::start(domain::FrameCallback onFrame) {
    std::lock_guard<std::mutex> lifecycle(m_impl->lifecycleMutex);
    if (m_impl->pipeline != nullptr) {
        return;
    }
    gst_init(nullptr, nullptr);
    m_impl->onFrame = std::move(onFrame);

    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(m_pipelineDescription.c_str(), &error);
    if (pipeline == nullptr || error != nullptr) {
        const std::string message = error != nullptr ? error->message : "unknown";
        if (pipeline != nullptr) {
            gst_object_unref(pipeline);
        }
        if (error != nullptr) {
            g_error_free(error);
        }
        // Tipicamente x264enc ausente (gstreamer1.0-plugins-ugly).
        throw domain::FrameSourceError("invalid pipeline [" + m_pipelineDescription + "]: " + message);
    }
    m_impl->pipeline = pipeline;
    m_impl->appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "ods_src");
    m_impl->appsink = gst_bin_get_by_name(GST_BIN(pipeline), "ods_sink");

    GstBus* bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_message, m_impl, nullptr);
    gst_object_unref(bus);

    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = on_encoded_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_impl->appsink), &callbacks, m_impl, nullptr);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        const std::string reason = m_impl->error();
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(m_impl->appsrc);
        gst_object_unref(m_impl->appsink);
        gst_object_unref(pipeline);
        m_impl->pipeline = m_impl->appsrc = m_impl->appsink = nullptr;
        throw domain::FrameSourceError("P4 encoder pipeline could not reach PLAYING: " + reason);
    }
}

void P4RawFrameEncoderSource::stop() {
    std::lock_guard<std::mutex> lifecycle(m_impl->lifecycleMutex);
    if (m_impl->pipeline == nullptr) {
        return;
    }
    // NULL espera a thread de streaming sair do callback; quadros ainda na
    // fila do encoder sao descartados com a captura.
    gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
    gst_object_unref(m_impl->appsrc);
    gst_object_unref(m_impl->appsink);
    gst_object_unref(m_impl->pipeline);
    m_impl->pipeline = m_impl->appsrc = m_impl->appsink = nullptr;

    std::lock_guard<std::mutex> lock(m_impl->pendingMutex);
    m_impl->pending.clear();
    m_impl->nextRunningTime = 0;
    m_impl->lastSessionId.clear();
}

bool P4RawFrameEncoderSource::isRunning() const {
    std::lock_guard<std::mutex> lifecycle(m_impl->lifecycleMutex);
    return m_impl->pipeline != nullptr;
}

void P4RawFrameEncoderSource::submit(
    const domain::P4FrameMetadata& metadata,
    const std::uint8_t* data,
    std::size_t length
) {
    rejectIfInvalid(metadata, length);
    if (data == nullptr) {
        throw domain::InvalidRawFrameError("frame without pixel data");
    }

    std::lock_guard<std::mutex> lifecycle(m_impl->lifecycleMutex);
    if (m_impl->pipeline == nullptr) {
        throw domain::FrameSourceError("P4 encoder source is not started");
    }
    if (const std::string error = m_impl->error(); !error.empty()) {
        throw domain::FrameSourceError("P4 encoder pipeline failed: " + error);
    }

    guint64 queuedBytes = 0;
    g_object_get(m_impl->appsrc, "current-level-bytes", &queuedBytes, nullptr);
    if (queuedBytes >= m_impl->maxQueuedBytes) {
        // Encoder atrasado: segurar a thread de P4 travaria a captura de todos
        // os consumidores; descartar sem contar violaria B2. Conta e registra.
        const std::uint64_t dropped = m_impl->droppedTotal.fetch_add(1) + 1;
        if (dropped == 1 || dropped % 30 == 0) {
            std::fprintf(stderr, "[S4.1] encoder behind; raw frame dropped (%llu so far)\n",
                         static_cast<unsigned long long>(dropped));
        }
        return;
    }

    GstClockTime runningTime = 0;
    bool sessionChanged = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->pendingMutex);
        runningTime = m_impl->nextRunningTime;
        m_impl->nextRunningTime += m_impl->frameDuration;
        sessionChanged = !m_impl->lastSessionId.empty() && m_impl->lastSessionId != metadata.sessionId;
        m_impl->lastSessionId = metadata.sessionId;
        // Registrado ANTES do push: o access unit pode sair do encoder antes de
        // gst_app_src_push_buffer retornar.
        m_impl->pending.push_back({runningTime, metadata});
    }
    if (sessionChanged) {
        request_keyframe_at(m_impl->appsrc, runningTime);
    }

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, length, nullptr);
    gst_buffer_fill(buffer, 0, data, length);
    GST_BUFFER_PTS(buffer) = runningTime;
    GST_BUFFER_DURATION(buffer) = m_impl->frameDuration;
    m_impl->submittedTotal.fetch_add(1);
    if (gst_app_src_push_buffer(GST_APP_SRC(m_impl->appsrc), buffer) != GST_FLOW_OK) {
        throw domain::FrameSourceError("appsrc refused the raw frame (pipeline flushing or stopped)");
    }
}

std::uint64_t P4RawFrameEncoderSource::framesSubmittedTotal() const noexcept {
    return m_impl->submittedTotal.load();
}

std::uint64_t P4RawFrameEncoderSource::framesDroppedTotal() const noexcept {
    return m_impl->droppedTotal.load();
}

} // namespace ods::s4::infrastructure

#else // sem GStreamer: validacao continua disponivel, codificacao nao

namespace ods::s4::infrastructure {

struct P4RawFrameEncoderSource::Impl {};

P4RawFrameEncoderSource::P4RawFrameEncoderSource(
    std::string sourceId,
    domain::RawVideoFormat format,
    SoftwareH264EncoderSettings encoder
) : m_sourceId(std::move(sourceId)),
    m_format(format),
    m_encoder(encoder),
    m_pipelineDescription(raw_to_h264_chain(m_format, m_encoder)),
    m_impl(nullptr) {}

P4RawFrameEncoderSource::~P4RawFrameEncoderSource() = default;

void P4RawFrameEncoderSource::start(domain::FrameCallback) {
    throw domain::FrameSourceError("built without GStreamer: rebuild with -DODS_S4_WITH_GSTREAMER=ON");
}

void P4RawFrameEncoderSource::stop() {}

bool P4RawFrameEncoderSource::isRunning() const { return false; }

void P4RawFrameEncoderSource::submit(const domain::P4FrameMetadata& metadata, const std::uint8_t*, std::size_t length) {
    rejectIfInvalid(metadata, length);
    throw domain::FrameSourceError("built without GStreamer: rebuild with -DODS_S4_WITH_GSTREAMER=ON");
}

std::uint64_t P4RawFrameEncoderSource::framesSubmittedTotal() const noexcept { return 0; }

std::uint64_t P4RawFrameEncoderSource::framesDroppedTotal() const noexcept { return 0; }

} // namespace ods::s4::infrastructure

#endif
