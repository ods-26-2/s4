#pragma once

// Carimbo de captura de um buffer GStreamer. So existe quando o S4 e compilado
// com GStreamer; fica num header proprio para que o teste consiga montar um
// GstBuffer e verificar a regra sem subir um pipeline inteiro.
#if defined(ODS_S4_WITH_GSTREAMER)

#include <optional>

#include <gst/gst.h>

#include "s4/domain/services/frame_source.hpp"

namespace ods::s4::infrastructure {

// Referencia GStreamer para tempo Unix em UTC (GstReferenceTimestampMeta).
inline constexpr const char* kUnixTimestampReference = "timestamp/x-unix";

struct CaptureInstant {
    domain::Nanoseconds captureTsNs{0};
    domain::CaptureTimeBase timeBase{domain::CaptureTimeBase::SourceClock};
};

// Ordem de preferencia:
//   1. GstReferenceTimestampMeta "timestamp/x-unix": o captured_at oficial,
//      carimbado por quem capturou (P4/P6) e preservado por x264enc/h264parse;
//   2. base_time do pipeline + DTS (ordem de decodificacao, que e a ordem em
//      que o buffer guarda: com B-frames o PTS chega fora de ordem);
//   3. base_time + PTS, para fontes sem DTS.
// Sem nenhum dos tres o quadro nao tem lugar na linha do tempo: nullopt.
[[nodiscard]] std::optional<CaptureInstant> capture_instant_of(
    GstClockTime pipelineBaseTime,
    GstBuffer* buffer
);

} // namespace ods::s4::infrastructure

#endif // ODS_S4_WITH_GSTREAMER
