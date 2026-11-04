#include "s4/infrastructure/b3_bus/envelope.hpp"

#include "s4/infrastructure/hashing/sha256_hasher.hpp"

#ifdef _WIN32
#include <rpc.h>
#else
#include <uuid/uuid.h>
#endif
#include <cctype>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace ods::s4::infrastructure {

nlohmann::json envelope_to_json(const B3Envelope& env) {
    nlohmann::json j;
    j["message_type"] = env.messageType;
    j["message_id"] = env.messageId;
    j["schema"] = env.schema;
    j["schema_version"] = env.schemaVersion;
    j["producer"] = env.producer;
    j["published_at"] = env.publishedAt;
    if (env.correlationId) j["correlation_id"] = *env.correlationId;
    if (env.capturedAt) j["captured_at"] = *env.capturedAt;
    if (env.sourceId) j["source_id"] = *env.sourceId;
    if (env.sessionId) j["session_id"] = *env.sessionId;
    if (env.sequence) j["sequence"] = *env.sequence;
    if (env.artifactVersion) j["artifact_version"] = *env.artifactVersion;
    if (env.target) j["target"] = *env.target;
    j["payload"] = env.payload;
    return j;
}

B3Envelope envelope_from_json(const nlohmann::json& j) {
    B3Envelope env;
    env.messageType = j.value("message_type", "");
    env.messageId = j.value("message_id", "");
    env.schema = j.value("schema", "");
    env.schemaVersion = j.value("schema_version", "");
    env.producer = j.value("producer", "");
    env.publishedAt = j.value("published_at", "");
    if (j.contains("correlation_id") && !j["correlation_id"].is_null()) env.correlationId = j["correlation_id"].get<std::string>();
    if (j.contains("captured_at") && !j["captured_at"].is_null()) env.capturedAt = j["captured_at"].get<std::string>();
    if (j.contains("source_id") && !j["source_id"].is_null()) env.sourceId = j["source_id"].get<std::string>();
    if (j.contains("session_id") && !j["session_id"].is_null()) env.sessionId = j["session_id"].get<std::string>();
    if (j.contains("sequence") && !j["sequence"].is_null()) env.sequence = j["sequence"].get<std::int64_t>();
    if (j.contains("artifact_version") && !j["artifact_version"].is_null()) env.artifactVersion = j["artifact_version"].get<std::string>();
    if (j.contains("target") && !j["target"].is_null()) env.target = j["target"].get<std::string>();
    env.payload = j.value("payload", nlohmann::json::object());
    return env;
}

std::string generate_message_id() {
#ifdef _WIN32
    UUID uuid;
    UuidCreate(&uuid);
    RPC_CSTR str;
    UuidToStringA(&uuid, &str);
    std::string s(reinterpret_cast<char*>(str));
    RpcStringFreeA(&str);
    return s;
#else
    uuid_t uuid;
    char buffer[37];
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, buffer);
    return std::string(buffer);
#endif
}

std::string now_iso8601_utc() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return out.str();
}

namespace {

bool is_hex(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

char hex_digit(int value) {
    return static_cast<char>(value < 10 ? ('0' + value) : ('a' + value - 10));
}

std::string lowercase(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string format_uuid(const std::string& hex32) {
    std::string uuid;
    uuid.reserve(36);
    for (std::size_t i = 0; i < 32; ++i) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            uuid.push_back('-');
        }
        uuid.push_back(hex32[i]);
    }
    return uuid;
}

} // namespace

bool is_uuid(const std::string& value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
        } else if (!is_hex(value[i])) {
            return false;
        }
    }
    return true;
}

std::string normalize_uuid(const std::string& value) {
    if (is_uuid(value)) {
        return lowercase(value);
    }
    if (value.empty()) {
        return generate_message_id();
    }

    // UUID v4 deterministico: mesma sessao/campo gera sempre o mesmo valor,
    // sem depender de estado em memoria (sobrevive a reinicio do processo).
    const std::vector<std::uint8_t> bytes(value.begin(), value.end());
    std::string hex = compute_sha256_hex(bytes).substr(0, 32);
    hex[12] = '4';
    const int variant = static_cast<int>(std::stoi(std::string(1, hex[16]), nullptr, 16));
    hex[16] = hex_digit((variant & 0x3) | 0x8);
    return format_uuid(hex);
}

std::int64_t next_sequence(const std::string& producer, const std::string& sessionId) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::int64_t> sequences;
    std::lock_guard<std::mutex> lock(mutex);
    const std::string key = producer + '\x1f' + sessionId;
    return ++sequences[key];
}

} // namespace ods::s4::infrastructure
