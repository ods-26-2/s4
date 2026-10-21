#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace ods::s4::application {

inline std::string to_iso8601(std::chrono::system_clock::time_point timestamp) {
    using namespace std::chrono;

    const auto millis = duration_cast<milliseconds>(timestamp.time_since_epoch());
    const std::time_t rawTime = system_clock::to_time_t(timestamp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &rawTime);
#else
    gmtime_r(&rawTime, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    out << '.' << std::setw(3) << std::setfill('0') << (millis.count() % 1000) << 'Z';
    return out.str();
}

} // namespace ods::s4::application