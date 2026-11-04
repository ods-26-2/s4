#include "s4/infrastructure/b3_bus/kafka_config.hpp"

#include <cstdlib>

namespace ods::s4::infrastructure {

namespace {

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

} // namespace

KafkaEventTransportConfig kafka_config_from_env() {
    KafkaEventTransportConfig config;
    const auto brokers = env_value("ODS_S4_KAFKA_BROKERS");
    if (!brokers.empty()) {
        config.brokers = brokers;
    }
    config.groupId = env_value("ODS_S4_KAFKA_GROUP_ID");
    if (config.groupId.empty()) {
        config.groupId = "s4-media-evidence";
    }
    config.clientId = env_value("ODS_S4_KAFKA_CLIENT_ID");
    if (config.clientId.empty()) {
        config.clientId = "s4";
    }
    config.securityProtocol = env_value("ODS_S4_KAFKA_SECURITY_PROTOCOL");
    config.saslMechanism = env_value("ODS_S4_KAFKA_SASL_MECHANISM");
    config.saslUsername = env_value("ODS_S4_KAFKA_SASL_USERNAME");
    config.saslPassword = env_value("ODS_S4_KAFKA_SASL_PASSWORD");
    config.sslCaLocation = env_value("ODS_S4_KAFKA_SSL_CA_LOCATION");
    return config;
}

} // namespace ods::s4::infrastructure
