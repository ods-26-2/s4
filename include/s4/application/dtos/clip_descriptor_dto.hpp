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
};

} // namespace ods::s4::application
