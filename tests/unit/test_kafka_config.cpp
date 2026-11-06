// Configuracao do barramento B3: o S4 nao embute broker. O endereco e as
// credenciais vem do ambiente fornecido pela equipe de integracao.
#include "tests/ods_check.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include "s4/infrastructure/b3_bus/kafka_config.hpp"

using namespace ods::s4::infrastructure;

namespace {

void clear_kafka_environment() {
    unsetenv("ODS_S4_KAFKA_BROKERS");
    unsetenv("ODS_S4_KAFKA_GROUP_ID");
    unsetenv("ODS_S4_KAFKA_CLIENT_ID");
    unsetenv("ODS_S4_KAFKA_SECURITY_PROTOCOL");
    unsetenv("ODS_S4_KAFKA_SASL_MECHANISM");
    unsetenv("ODS_S4_KAFKA_SASL_USERNAME");
    unsetenv("ODS_S4_KAFKA_SASL_PASSWORD");
    unsetenv("ODS_S4_KAFKA_SSL_CA_LOCATION");
}

} // namespace

void test_deve_usar_broker_padrao_quando_nao_ha_ambiente() {
    clear_kafka_environment();
    const auto config = kafka_config_from_env();
    ODS_CHECK(config.enabled());
    ODS_CHECK(config.brokers == "172.18.4.56:9092");
    ODS_CHECK(config.groupId == "s4-media-evidence");
    ODS_CHECK(config.clientId == "s4");
}

void test_deve_ler_broker_e_identidade_do_ambiente() {
    clear_kafka_environment();
    setenv("ODS_S4_KAFKA_BROKERS", "broker.local:9092", 1);
    setenv("ODS_S4_KAFKA_GROUP_ID", "S4-jetson", 1);
    setenv("ODS_S4_KAFKA_CLIENT_ID", "s4-edge", 1);

    const auto config = kafka_config_from_env();
    ODS_CHECK(config.enabled());
    ODS_CHECK(config.brokers == "broker.local:9092");
    ODS_CHECK(config.groupId == "S4-jetson");
    ODS_CHECK(config.clientId == "s4-edge");
}

void test_deve_repassar_seguranca_sem_alterar_segredo() {
    clear_kafka_environment();
    setenv("ODS_S4_KAFKA_BROKERS", "broker.local:9093", 1);
    setenv("ODS_S4_KAFKA_SECURITY_PROTOCOL", "SASL_SSL", 1);
    setenv("ODS_S4_KAFKA_SASL_MECHANISM", "SCRAM-SHA-512", 1);
    setenv("ODS_S4_KAFKA_SASL_USERNAME", "s4", 1);
    setenv("ODS_S4_KAFKA_SASL_PASSWORD", "segredo", 1);
    setenv("ODS_S4_KAFKA_SSL_CA_LOCATION", "/etc/ssl/ca.pem", 1);

    const auto config = kafka_config_from_env();
    ODS_CHECK(config.securityProtocol == "SASL_SSL");
    ODS_CHECK(config.saslMechanism == "SCRAM-SHA-512");
    ODS_CHECK(config.saslUsername == "s4");
    ODS_CHECK(config.saslPassword == "segredo");
    ODS_CHECK(config.sslCaLocation == "/etc/ssl/ca.pem");
}

int main() {
    std::cout << "Running S4 Kafka configuration tests...\n";
    test_deve_usar_broker_padrao_quando_nao_ha_ambiente();
    test_deve_ler_broker_e_identidade_do_ambiente();
    test_deve_repassar_seguranca_sem_alterar_segredo();
    std::cout << "All S4 Kafka configuration tests passed successfully!\n";
    return 0;
}
