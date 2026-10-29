#pragma once

#include <chrono>
#include "s4/domain/errors/domain_error.hpp"

namespace ods::s4::domain {

class TimeWindow {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    TimeWindow(TimePoint startTime, TimePoint endTime)
        : m_startTime(startTime), m_endTime(endTime) {
        if (m_startTime >= m_endTime) {
            throw InvalidTimeWindowError("start_time must be strictly before end_time");
        }
    }

    [[nodiscard]] TimePoint startTime() const noexcept { return m_startTime; }
    [[nodiscard]] TimePoint endTime() const noexcept { return m_endTime; }

    [[nodiscard]] double durationSeconds() const noexcept {
        return std::chrono::duration<double>(m_endTime - m_startTime).count();
    }

private:
    TimePoint m_startTime;
    TimePoint m_endTime;
};

} // namespace ods::s4::domain
