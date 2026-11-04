#pragma once

#include <cstdint>
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

// Confere se o valor e um UUID canonico (8-4-4-4-12, hexadecimal).
bool is_uuid(const std::string& value);

// Devolve o UUID em minusculas quando o valor ja e valido. Caso contrario,
// deriva um UUID deterministico do texto (para nao trocar de sessao a cada
// mensagem) e, se o valor for vazio, gera um UUID aleatorio.
std::string normalize_uuid(const std::string& value);

// Sequence por produtor + sessao, conforme o envelope B1. Thread-safe e
// compartilhado por todos os handlers do processo.
std::int64_t next_sequence(const std::string& producer, const std::string& sessionId);

} // namespace ods::s4::infrastructure
