#pragma once

#include <string>

namespace ods::s4::infrastructure {

// Configuracao do transporte Kafka. O broker padrao e o endpoint da equipe de
// integracao; quem opera o barramento pode sobrescreve-lo por ambiente ou flag.
// Para operar sem barramento use --kafka-disabled.
struct KafkaEventTransportConfig {
    std::string brokers = "200.17.66.56:9093";
    std::string groupId = "s4-media-evidence";
    std::string clientId = "s4";
    std::string securityProtocol;
    std::string saslMechanism;
    std::string saslUsername;
    std::string saslPassword;
    std::string sslCaLocation;

    [[nodiscard]] bool enabled() const { return !brokers.empty(); }
};

// Le a configuracao de ODS_S4_KAFKA_*. Variaveis ausentes ficam vazias ou com
// os padroes acima. Segredos (usuario/senha) nunca devem ser logados.
KafkaEventTransportConfig kafka_config_from_env();

} // namespace ods::s4::infrastructure
