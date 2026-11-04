// Envelope B1: campos que a integracao valida (captured_at, sequence,
// session_id/clip_id em UUID) e correlation_id do ods.s4.clip.
#include "tests/ods_check.hpp"

#include <iostream>
#include <string>

#include "s4/infrastructure/b3_bus/envelope.hpp"

using namespace ods::s4::infrastructure;

void test_deve_reconhecer_uuid_canonico() {
    ODS_CHECK(is_uuid("123e4567-e89b-12d3-a456-426614174000"));
    ODS_CHECK(is_uuid("123E4567-E89B-12D3-A456-426614174000"));
    ODS_CHECK(!is_uuid("exec-demo-1"));
    ODS_CHECK(!is_uuid("123e4567e89b12d3a456426614174000"));
    ODS_CHECK(!is_uuid(""));
}

void test_deve_normalizar_sessao_nao_uuid_de_forma_estavel() {
    const std::string first = normalize_uuid("exec-demo-1");
    const std::string second = normalize_uuid("exec-demo-1");
    ODS_CHECK(is_uuid(first));
    ODS_CHECK(first == second);
    ODS_CHECK(first != normalize_uuid("exec-demo-2"));
    ODS_CHECK(normalize_uuid("123E4567-E89B-12D3-A456-426614174000") ==
              "123e4567-e89b-12d3-a456-426614174000");
}

void test_deve_avancar_sequence_por_produtor_e_sessao() {
    const auto a1 = next_sequence("S4-test-a", "sessao-1");
    const auto a2 = next_sequence("S4-test-a", "sessao-1");
    const auto b1 = next_sequence("S4-test-a", "sessao-2");
    ODS_CHECK(a2 == a1 + 1);
    ODS_CHECK(b1 == 1);
}

void test_deve_serializar_campos_obrigatorios_do_clip() {
    B3Envelope env;
    env.messageType = "event";
    env.messageId = generate_message_id();
    env.schema = "ods.s4.clip";
    env.schemaVersion = "1.0";
    env.producer = "S4";
    env.publishedAt = now_iso8601_utc();
    env.capturedAt = "2026-10-29T12:00:00.000Z";
    env.sessionId = normalize_uuid("exec-demo-1");
    env.correlationId = "evt-123";
    env.sequence = next_sequence("S4", *env.sessionId);

    const auto json = envelope_to_json(env);
    ODS_CHECK(json.contains("captured_at"));
    ODS_CHECK(json.contains("sequence"));
    ODS_CHECK(json.contains("session_id"));
    ODS_CHECK(json.contains("correlation_id"));
    ODS_CHECK(is_uuid(json["session_id"].get<std::string>()));
    ODS_CHECK(json["correlation_id"].get<std::string>() == "evt-123");
    ODS_CHECK(json["captured_at"].get<std::string>() == "2026-10-29T12:00:00.000Z");
}

int main() {
    std::cout << "Running S4 envelope tests...\n";
    test_deve_reconhecer_uuid_canonico();
    test_deve_normalizar_sessao_nao_uuid_de_forma_estavel();
    test_deve_avancar_sequence_por_produtor_e_sessao();
    test_deve_serializar_campos_obrigatorios_do_clip();
    std::cout << "All S4 envelope tests passed successfully!\n";
    return 0;
}
