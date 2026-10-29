#pragma once

#include <cstddef>
#include <string>

#include "s4/domain/value_objects/p4_frame_metadata.hpp"

namespace ods::s4::infrastructure {

// Presets de velocidade do x264 aceitos pelo S4. Os mais lentos que "fast" nao
// cabem na CPU da Orin Nano com a inferencia rodando junto.
enum class X264SpeedPreset { Ultrafast, Superfast, Veryfast, Faster, Fast };

// Encoder H.264 por SOFTWARE (x264enc / libx264) do S4.1.
//
// Por que software: a Jetson Orin Nano nao tem o motor NVENC, entao o
// nvv4l2h264enc que o pipeline usava nao existe na placa; a NVIDIA orienta
// codificar com x264enc/libx264. E guardar quadros crus nao e alternativa:
// 1080p NV12 tem ~3,1 MB por quadro, mais de 1 GB por camera para 13 s.
//
// Ponto de partida da NVIDIA para camera a 30 fps: GOP 30, sem B-frames,
// 1 quadro de referencia, presets ultrafast a veryfast = 18-22% de um nucleo.
// Os padroes abaixo seguem isso; o numero final sai da medicao na placa com a
// inferencia rodando (tools/measure_encoder.cpp).
//
// Fixos de proposito (nao configuraveis):
//   * bframes=0      o buffer guarda em ordem de decodificacao = ordem de
//                    captura; com B-frames o PTS sai fora de ordem;
//   * tune=zerolatency sem lookahead: o quadro sai do encoder no mesmo
//                    instante em que entra, e o pos-evento chega ao buffer sem
//                    atraso extra (com lookahead seriam dezenas de quadros);
//   * byte-stream    Annex-B, com SPS/PPS repetidos em todo IDR — um trecho
//                    que comeca no meio do stream continua decodificavel.
struct SoftwareH264EncoderSettings {
    // ultrafast: o mais barato. Medido com tools/measure_encoder.cpp (1080p30,
    // 4 Mbps, cena com ruido, Xeon 2,1 GHz — NAO a placa): ultrafast 41%,
    // superfast 74%, veryfast 121% de um nucleo. A CPU da Orin Nano e dividida
    // com a inferencia, entao o padrao e o seguro; a medicao na placa decide se
    // da para subir (--encoder-preset).
    X264SpeedPreset speedPreset{X264SpeedPreset::Ultrafast};
    // Bitrate alvo. E o MESMO numero que dimensiona a arena do ring buffer
    // (RingBufferConfig::bitrateBps): 4 Mbps a 1080p30 cabem em ~21 MB/30 s.
    std::size_t bitrateBps{4000000};
    // GOP em quadros: 30 a 30 fps = um keyframe por segundo, o que limita a
    // ~1 s o recuo do alinhamento a keyframe na extracao.
    int keyframeInterval{30};
    int referenceFrames{1};
    // 0 = o x264 escolhe. Na placa, limitar deixa nucleos livres para a inferencia.
    int threads{0};

    // Lanca InvalidEncoderSettingsError se algum campo estiver fora da faixa.
    void validate() const;

    // Trecho de pipeline GStreamer: "x264enc speed-preset=... bitrate=... ...".
    [[nodiscard]] std::string gstElement() const;

    [[nodiscard]] double gopSeconds(double fps) const {
        return fps > 0.0 ? static_cast<double>(keyframeInterval) / fps : 0.0;
    }
};

[[nodiscard]] const char* to_string(X264SpeedPreset preset) noexcept;

// "ultrafast", "superfast", "veryfast", "faster" ou "fast"; lanca
// InvalidEncoderSettingsError para qualquer outro nome.
[[nodiscard]] X264SpeedPreset parse_x264_speed_preset(const std::string& name);

// Caps GStreamer do stream cru de P4: "video/x-raw,format=NV12,width=...".
[[nodiscard]] std::string raw_video_caps(const domain::RawVideoFormat& format);

// Do quadro cru ao access unit que o appsink do S4.1 espera:
//   [videoconvert !] x264enc ... ! h264parse config-interval=-1 ! caps Annex-B/au
// NV12 entra direto no x264enc; RGB, BGR e GRAY passam por videoconvert para
// I420, que qualquer decoder de player abre.
[[nodiscard]] std::string raw_to_h264_chain(
    const domain::RawVideoFormat& format,
    const SoftwareH264EncoderSettings& encoder
);

} // namespace ods::s4::infrastructure
