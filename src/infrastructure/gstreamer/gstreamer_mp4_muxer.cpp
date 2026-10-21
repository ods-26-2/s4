#include "s4/infrastructure/gstreamer/gstreamer_mp4_muxer.hpp"

#include "s4/infrastructure/media/iso_bmff_timestamps.hpp"


namespace ods::s4::infrastructure {

namespace {

// h264parse preenche width/height/framerate a partir do SPS que ele le, entao o
// caps de entrada so precisa fixar o formato do stream. Fixar dimensoes aqui
// seria mentir: o RingBufferConfig tem width/height de configuracao, que nao
// precisa bater com o stream que a camera entregou.
constexpr const char* kInputCaps =
    "video/x-h264,stream-format=byte-stream,alignment=au";

} // namespace

std::string GStreamerMp4Muxer::pipelineDescription() {
    return std::string("appsrc name=ods_mux_src caps=\"") + kInputCaps +
           "\" format=time ! "
           "h264parse config-interval=-1 ! "
           "mp4mux name=ods_mux_muxer faststart=true ! "
           "appsink name=ods_mux_sink emit-signals=true sync=false max-buffers=0";
}

#if !defined(ODS_S4_WITH_GSTREAMER)

struct GStreamerMp4Muxer::Impl {};

GStreamerMp4Muxer::GStreamerMp4Muxer(bool normalizeTimestamps)
    : m_normalizeTimestamps(normalizeTimestamps), m_impl(new Impl()) {}

GStreamerMp4Muxer::~GStreamerMp4Muxer() { delete m_impl; }

std::vector<std::uint8_t> GStreamerMp4Muxer::mux(const application::ExtractedSegment&) {
    // Sem GStreamer nao existe caminho honesto: escrever os bytes Annex-B com
    // extensao .mp4 seria o defeito que esta classe existe para eliminar.
    throw application::MediaMuxError(
        "GStreamer is disabled; cannot produce an MP4 container "
        "(rebuild with -DODS_S4_WITH_GSTREAMER=ON)"
    );
}

#else

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

GstFlowReturn on_new_sample(GstAppSink* sink, gpointer userData) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo info;
    if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }
    const auto* begin = static_cast<const std::uint8_t*>(info.data);
    out->insert(out->end(), begin, begin + info.size);
    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

} // namespace

struct GStreamerMp4Muxer::Impl {
    GstElement* pipeline{nullptr};
    GstElement* source{nullptr};
    GstElement* sink{nullptr};
};

GStreamerMp4Muxer::GStreamerMp4Muxer(bool normalizeTimestamps)
    : m_normalizeTimestamps(normalizeTimestamps), m_impl(new Impl()) {}

GStreamerMp4Muxer::~GStreamerMp4Muxer() {
    if (m_impl->pipeline != nullptr) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        gst_object_unref(m_impl->pipeline);
    }
    delete m_impl;
}

std::vector<std::uint8_t> GStreamerMp4Muxer::mux(
    const application::ExtractedSegment& segment
) {
    if (segment.frames.empty()) {
        throw application::MediaMuxError("cannot mux a segment with no frames");
    }

    gst_init(nullptr, nullptr);

    if (m_impl->pipeline != nullptr) {
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        gst_object_unref(m_impl->pipeline);
        m_impl->pipeline = nullptr;
    }

    GError* error = nullptr;
    m_impl->pipeline = gst_parse_launch(pipelineDescription().c_str(), &error);
    if (m_impl->pipeline == nullptr || error != nullptr) {
        const std::string message = error != nullptr ? error->message : "unknown";
        if (m_impl->pipeline != nullptr) {
            gst_object_unref(m_impl->pipeline);
            m_impl->pipeline = nullptr;
        }
        if (error != nullptr) {
            g_error_free(error);
        }
        throw application::MediaMuxError("invalid mux pipeline: " + message);
    }

    m_impl->source = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "ods_mux_src");
    m_impl->sink = gst_bin_get_by_name(GST_BIN(m_impl->pipeline), "ods_mux_sink");
    if (m_impl->source == nullptr || m_impl->sink == nullptr) {
        throw application::MediaMuxError("mux pipeline is missing appsrc or appsink");
    }

    auto* appsrc = GST_APP_SRC(m_impl->source);
    std::vector<std::uint8_t> container;
    g_signal_connect(m_impl->sink, "new-sample", G_CALLBACK(on_new_sample), &container);

    g_object_set(appsrc, "format", GST_FORMAT_TIME, "is-live", FALSE, "block", TRUE, nullptr);

    if (gst_element_set_state(m_impl->pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        throw application::MediaMuxError("mux pipeline refused to start");
    }

    // Empurra na ordem em que os quadros chegaram (ordem de captura =
    // ordem de decodificacao). O mp4mux escreve as tabelas de amostra a
    // partir de DTS crescentes, entao a linha do tempo e ACUMULADA a partir
    // dos deltas: usar o delta como se fosse o timestamp produziria uma
    // sequencia nao monotona (0, 33ms, 33ms, 33ms...) e o container sairia
    // invalido.
    const std::size_t frameCount = segment.frames.size();
    std::vector<GstClockTime> deltas(frameCount, 0);
    GstClockTime lastDelta = 0;
    for (std::size_t i = 1; i < frameCount; ++i) {
        const auto previous = segment.frames[i - 1].captureTsNs;
        const auto current = segment.frames[i].captureTsNs;
        // Deltas negativos (relogio retrocedendo) viram 0 em vez de gerar um
        // DTS invalido.
        deltas[i] = current > previous
            ? static_cast<GstClockTime>(current - previous)
            : static_cast<GstClockTime>(0);
        if (deltas[i] > 0) {
            lastDelta = deltas[i];
        }
    }

    // A linha do tempo comeca em 1 ns, e nao em 0: GST_CLOCK_TIME_NONE e
    // exatamente 0, entao um primeiro sample com PTS 0 e lido pelo mp4mux como
    // "buffer sem PTS" e o container inteiro e recusado
    // ("Buffer has no PTS"). Um tick de deslocamento nao muda a duracao nem a
    // ordem; so evita o valor reservado.
    constexpr GstClockTime kFirstSampleTs = 1;
    GstClockTime runningTs = kFirstSampleTs;
    for (std::size_t i = 0; i < frameCount; ++i) {
        const auto& frame = segment.frames[i];
        if (frame.data.empty()) {
            continue;
        }
        runningTs += deltas[i];

        GstBuffer* buffer = gst_buffer_new_allocate(
            nullptr,
            static_cast<gulong>(frame.data.size()),
            nullptr
        );
        // gst_buffer_fill e a forma correta de popular um buffer recem-alocado
        // (preserva o padding que o GStreamer garante para alguns formatos).
        gst_buffer_fill(buffer, 0, frame.data.data(), frame.data.size());

        // A duracao do amostra e o intervalo ate o proximo; no ultimo quadro
        // usa o ultimo delta conhecido, porque um sample de duracao 0 faz o
        // mp4mux calcular uma duracao total errada.
        GstClockTime duration = (i + 1 < frameCount) ? deltas[i + 1] : lastDelta;
        if (duration == 0) {
            duration = static_cast<GstClockTime>(domain::kNanosecondsPerSecond / 30);
        }

        GST_BUFFER_PTS(buffer) = runningTs;
        GST_BUFFER_DTS(buffer) = runningTs;
        GST_BUFFER_DURATION(buffer) = duration;

        const GstFlowReturn ret = gst_app_src_push_buffer(appsrc, buffer);
        if (ret != GST_FLOW_OK) {
            gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
            throw application::MediaMuxError(
                "mux pipeline rejected frame " + std::to_string(i) +
                ": flow return " + std::to_string(ret)
            );
        }
    }

    // O EOS e o que faz o mp4mux escrever o moov e finalizar o container.
    gst_app_src_end_of_stream(appsrc);

    GstBus* bus = gst_element_get_bus(m_impl->pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus,
        10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)
    );
    if (message == nullptr) {
        gst_object_unref(bus);
        gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);
        throw application::MediaMuxError("mux pipeline timed out before finishing the container");
    }
    std::string failure;
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* busError = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &busError, &debug);
        // A mensagem do bus e a unica que diz QUAL elemento recusou o
        // stream; sem ela o erro chega como "nao produziu container", que
        // nao ajuda ninguem a diagnosticar.
        failure = busError != nullptr ? busError->message : "unknown GStreamer error";
        if (busError != nullptr) {
            g_error_free(busError);
        }
        if (debug != nullptr) {
            g_free(debug);
        }
    }
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(m_impl->pipeline, GST_STATE_NULL);

    if (!failure.empty()) {
        throw application::MediaMuxError("mux pipeline failed: " + failure);
    }
    if (container.empty()) {
        throw application::MediaMuxError(
            "mux pipeline finished without producing any container bytes"
        );
    }

    if (m_normalizeTimestamps) {
        // Feito DEPOIS do pipeline: o mp4mux grava a hora do muxing nos boxes
        // de cabecalho, e essa e a unica fonte de nao-determinismo no
        // container. Ver a discussao em iso_bmff_timestamps.hpp.
        normalize_iso_bmff_timestamps(container);
    }
    return container;
}

#endif // ODS_S4_WITH_GSTREAMER

} // namespace ods::s4::infrastructure
