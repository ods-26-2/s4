#include "s4/infrastructure/gstreamer/software_h264_encoder.hpp"

#include <cmath>

#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::infrastructure {

namespace {

constexpr int kBitsPerKilobit = 1000;
// Limites do proprio x264enc (propriedades key-int-max e ref).
constexpr int kMaxReferenceFrames = 16;
// Taxas NTSC (29,97 = 30000/1001) sao a unica fracao comum em camera.
constexpr long long kNtscDenominator = 1001;

const char* gst_raw_format(domain::RawPixelFormat format) {
    switch (format) {
        case domain::RawPixelFormat::NV12:
            return "NV12";
        case domain::RawPixelFormat::RGB:
            return "RGB";
        case domain::RawPixelFormat::BGR:
            return "BGR";
        case domain::RawPixelFormat::GRAY:
            return "GRAY8";
    }
    return "NV12";
}

std::string gst_framerate(double fps) {
    const double rounded = std::round(fps);
    if (std::fabs(fps - rounded) < 1e-6) {
        return std::to_string(static_cast<long long>(rounded)) + "/1";
    }
    const auto numerator = static_cast<long long>(std::llround(fps * kNtscDenominator));
    return std::to_string(numerator) + "/" + std::to_string(kNtscDenominator);
}

} // namespace

const char* to_string(X264SpeedPreset preset) noexcept {
    switch (preset) {
        case X264SpeedPreset::Ultrafast:
            return "ultrafast";
        case X264SpeedPreset::Superfast:
            return "superfast";
        case X264SpeedPreset::Veryfast:
            return "veryfast";
        case X264SpeedPreset::Faster:
            return "faster";
        case X264SpeedPreset::Fast:
            return "fast";
    }
    return "veryfast";
}

X264SpeedPreset parse_x264_speed_preset(const std::string& name) {
    for (const auto preset : {X264SpeedPreset::Ultrafast, X264SpeedPreset::Superfast,
                              X264SpeedPreset::Veryfast, X264SpeedPreset::Faster,
                              X264SpeedPreset::Fast}) {
        if (name == to_string(preset)) {
            return preset;
        }
    }
    throw domain::InvalidEncoderSettingsError(
        "unknown x264 speed preset '" + name + "' (use ultrafast, superfast, veryfast, faster or fast)"
    );
}

void SoftwareH264EncoderSettings::validate() const {
    if (bitrateBps < kBitsPerKilobit) {
        throw domain::InvalidEncoderSettingsError("encoder bitrate must be at least 1 kbps");
    }
    if (keyframeInterval <= 0) {
        // 0 seria "automatico" no x264 (ate 250 quadros entre keyframes): o
        // recuo do alinhamento a keyframe passaria de 8 s a 30 fps.
        throw domain::InvalidEncoderSettingsError("keyframe interval must be positive");
    }
    if (referenceFrames < 1 || referenceFrames > kMaxReferenceFrames) {
        throw domain::InvalidEncoderSettingsError("reference frames must be between 1 and 16");
    }
    if (threads < 0) {
        throw domain::InvalidEncoderSettingsError("encoder threads must not be negative");
    }
}

std::string SoftwareH264EncoderSettings::gstElement() const {
    validate();
    return std::string("x264enc speed-preset=") + to_string(speedPreset) +
           " tune=zerolatency bitrate=" + std::to_string(bitrateBps / kBitsPerKilobit) +
           " key-int-max=" + std::to_string(keyframeInterval) +
           " bframes=0 ref=" + std::to_string(referenceFrames) +
           " threads=" + std::to_string(threads) + " byte-stream=true";
}

std::string raw_video_caps(const domain::RawVideoFormat& format) {
    return std::string("video/x-raw,format=") + gst_raw_format(format.pixelFormat()) +
           ",width=" + std::to_string(format.width()) +
           ",height=" + std::to_string(format.height()) +
           ",framerate=" + gst_framerate(format.fps());
}

std::string raw_to_h264_chain(
    const domain::RawVideoFormat& format,
    const SoftwareH264EncoderSettings& encoder
) {
    std::string chain;
    if (format.pixelFormat() != domain::RawPixelFormat::NV12) {
        chain = "videoconvert ! video/x-raw,format=I420 ! ";
    }
    return chain + encoder.gstElement() +
           " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au";
}

} // namespace ods::s4::infrastructure
