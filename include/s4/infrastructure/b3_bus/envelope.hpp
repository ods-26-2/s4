#pragma once

#include <string>
#include <optional>
#include <chrono>
#include <nlohmann/json.hpp>

namespace ods::s4::infrastructure {

struct B3Envelope {
    std::string messageType;      // event, command, query, reply
    std::string messageId;
    std::string schema;
    std::string schemaVersion;
    std::string producer;
    std::string publishedAt;
    std::optional<std::string> correlationId;
    std::optional<std::string> capturedAt;
    std::optional<std::string> sourceId;
    std::optional<std::string> sessionId;
    std::optional<std::int64_t> sequence;
    std::optional<std::string> artifactVersion;
    std::optional<std::string> target;
    nlohmann::json payload = nlohmann::json::object();
};

nlohmann::json envelope_to_json(const B3Envelope& env);
B3Envelope envelope_from_json(const nlohmann::json& j);

std::string generate_message_id();
std::string now_iso8601_utc();

} // namespace ods::s4::infrastructure
