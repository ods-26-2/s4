#pragma once

#include <string>
#include <vector>

namespace ods::s4::application {

struct PointOfInterestDTO {
    double x{0.0};
    double y{0.0};
    std::string label;
};

struct ClipDescriptorDTO {
    std::string clipId;
    std::string fileUri;
    std::string startTimeIso;
    std::string endTimeIso;
    std::vector<PointOfInterestDTO> pointsOfInterest;
    std::string sha256Hash;
    bool isRetained{true};
    bool isLockedForAudit{false};
    std::string mediaType{"CLIP"};   // "CLIP" ou "SNAPSHOT" (6c)
    std::string expiresAtIso;        // prazo publicado; depois dele o clipe nao e servido
    // Falso quando o clipe ja foi expurgado ou o prazo venceu: o servidor
    // responde 410 em vez de entregar o descritor.
    bool isAvailable{true};
};

} // namespace ods::s4::application
