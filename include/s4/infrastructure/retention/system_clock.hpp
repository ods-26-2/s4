#pragma once

#include "s4/domain/services/clock.hpp"

namespace ods::s4::infrastructure {

class SystemClock : public domain::IClock {
public:
    [[nodiscard]] TimePoint now() const override { return std::chrono::system_clock::now(); }
};

} // namespace ods::s4::infrastructure
