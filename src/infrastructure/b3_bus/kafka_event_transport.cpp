#include "s4/infrastructure/b3_bus/kafka_event_transport.hpp"
#include "s4/infrastructure/b3_bus/envelope.hpp"

#ifdef ODS_S4_WITH_KAFKA
#include <nlohmann/json.hpp>
#endif

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace ods::s4::infrastructure {

namespace {

#ifdef ODS_S4_WITH_KAFKA
void set_required(RdKafka::Conf& conf, const char* key, const std::string& value) {
    std::string error;
    if (conf.set(key, value, error) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error(std::string("kafka: cannot set ") + key + ": " + error);
    }
}

// Repassa a configuracao de seguranca fornecida pela equipe que opera o
// barramento. Nada aqui e logado: usuario e senha nunca vao para o stdout.
void apply_security(RdKafka::Conf& conf, const KafkaEventTransportConfig& config) {
    if (!config.securityProtocol.empty()) {
        set_required(conf, "security.protocol", config.securityProtocol);
    }
    if (!config.saslMechanism.empty()) {
        set_required(conf, "sasl.mechanism", config.saslMechanism);
    }
    if (!config.saslUsername.empty()) {
        set_required(conf, "sasl.username", config.saslUsername);
    }
    if (!config.saslPassword.empty()) {
        set_required(conf, "sasl.password", config.saslPassword);
    }
    if (!config.sslCaLocation.empty()) {
        set_required(conf, "ssl.ca.location", config.sslCaLocation);
    }
}
#endif

} // namespace

#ifdef ODS_S4_WITH_KAFKA
void KafkaEventTransport::DeliveryReporter::dr_cb(RdKafka::Message& message) {
    if (message.err() != RdKafka::ERR_NO_ERROR) {
        failures.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "Kafka delivery failed: " << message.errstr() << std::endl;
    }
}
#endif

KafkaEventTransport::KafkaEventTransport(const KafkaEventTransportConfig& config)
    : m_config(config) {
#ifdef ODS_S4_WITH_KAFKA
    std::string errstr;
    m_conf = std::unique_ptr<RdKafka::Conf>(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!m_conf) {
        throw std::runtime_error("kafka: cannot create global configuration");
    }
    set_required(*m_conf, "bootstrap.servers", m_config.brokers);
    set_required(*m_conf, "group.id", m_config.groupId);
    set_required(*m_conf, "client.id", m_config.clientId);
    set_required(*m_conf, "auto.offset.reset", "earliest");
    set_required(*m_conf, "enable.auto.commit", "false");
    apply_security(*m_conf, m_config);

    m_consumer = std::unique_ptr<RdKafka::KafkaConsumer>(
        RdKafka::KafkaConsumer::create(m_conf.get(), errstr));
    if (!m_consumer) {
        throw std::runtime_error("kafka: cannot create consumer: " + errstr);
    }

    std::string producerErr;
    auto producerConf = std::unique_ptr<RdKafka::Conf>(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    if (!producerConf) {
        throw std::runtime_error("kafka: cannot create producer configuration");
    }
    set_required(*producerConf, "bootstrap.servers", m_config.brokers);
    set_required(*producerConf, "client.id", m_config.clientId + "-producer");
    if (producerConf->set("dr_cb", &m_deliveryReporter, producerErr) !=
        RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("kafka: cannot set delivery callback: " + producerErr);
    }
    apply_security(*producerConf, m_config);
    m_producer = std::unique_ptr<RdKafka::Producer>(
        RdKafka::Producer::create(producerConf.get(), producerErr));
    if (!m_producer) {
        throw std::runtime_error("kafka: cannot create producer: " + producerErr);
    }
#endif
}

KafkaEventTransport::~KafkaEventTransport() {
    stop();
}

void KafkaEventTransport::subscribe(const std::string& topic, MessageHandler handler) {
    m_subscriptions.push_back({topic, std::move(handler)});
#ifdef ODS_S4_WITH_KAFKA
    if (m_consumer) {
        std::vector<std::string> topics;
        for (const auto& s : m_subscriptions) topics.push_back(s.first);
        m_consumer->subscribe(topics);
    }
#endif
}

void KafkaEventTransport::publish(const std::string& topic, const B3Envelope& envelope) {
#ifdef ODS_S4_WITH_KAFKA
    if (!m_producer) return;
    // Garante o campo obrigatorio do B1 mesmo em mensagens montadas fora dos
    // handlers (replies, por exemplo): sequence por produtor + sessao.
    B3Envelope outgoing = envelope;
    if (!outgoing.sequence) {
        outgoing.sequence = next_sequence(outgoing.producer, outgoing.sessionId.value_or(""));
    }
    const auto j = envelope_to_json(outgoing);
    const std::string payload = j.dump();
    RdKafka::ErrorCode err = m_producer->produce(
        topic,
        RdKafka::Topic::PARTITION_UA,
        RdKafka::Producer::RK_MSG_COPY,
        const_cast<char*>(payload.c_str()),
        payload.size(),
        nullptr, 0,
        0,
        nullptr
    );
    if (err != RdKafka::ERR_NO_ERROR) {
        std::cerr << "Kafka produce failed: " << RdKafka::err2str(err) << std::endl;
    }
    // Serve os callbacks de entrega. O resultado definitivo chega no
    // DeliveryReporter e no flush do stop().
    m_producer->poll(0);
#else
    (void)topic;
    (void)envelope;
#endif
}

void KafkaEventTransport::start() {
    if (m_stopped.load()) {
        std::cerr << "Kafka transport cannot restart after stop()" << std::endl;
        return;
    }
    if (m_running.exchange(true)) return;
#ifdef ODS_S4_WITH_KAFKA
    m_thread = std::thread([this]{ consume_loop(); });
#else
    (void)0;
#endif
}

void KafkaEventTransport::stop() {
    // Idempotente: main e o destrutor chamam stop(); sem esta guarda o flush
    // de 5 s rodaria duas vezes no encerramento.
    if (m_stopped.exchange(true)) return;
    const bool wasRunning = m_running.exchange(false);
    if (wasRunning && m_thread.joinable()) {
        m_thread.join();
    }
#ifdef ODS_S4_WITH_KAFKA
    if (m_consumer) {
        m_consumer->close();
    }
    if (m_producer) {
        const auto pending = m_producer->flush(5000);
        if (pending != RdKafka::ERR_NO_ERROR) {
            std::cerr << "Kafka producer flush incomplete: "
                      << RdKafka::err2str(pending)
                      << " (delivery failures: "
                      << m_deliveryReporter.failures.load(std::memory_order_relaxed)
                      << ")" << std::endl;
        }
    }
#endif
}

void KafkaEventTransport::consume_loop() {
#ifdef ODS_S4_WITH_KAFKA
    if (!m_consumer) return;
    while (m_running.load()) {
        RdKafka::Message* raw = m_consumer->consume(1000);
        if (!raw) continue;
        std::unique_ptr<RdKafka::Message> msg(raw);
        if (msg->err() == RdKafka::ERR_NO_ERROR && msg->payload()) {
            m_allBrokersDownLogged.store(false);
            try {
                const std::string data(static_cast<const char*>(msg->payload()), msg->len());
                const auto j = nlohmann::json::parse(data);
                const auto envelope = envelope_from_json(j);
                const std::string topic = msg->topic_name();
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto& [t, handler] : m_subscriptions) {
                    if (t == topic) {
                        handler(topic, envelope);
                    }
                }
                m_consumer->commitSync(msg.get());
            } catch (const std::exception& e) {
                std::cerr << "Kafka message parse error: " << e.what() << std::endl;
                m_consumer->commitSync(msg.get());
            }
        } else if (msg->err() == RdKafka::ERR__ALL_BROKERS_DOWN &&
                   !m_allBrokersDownLogged.exchange(true)) {
            std::cerr << "Kafka: all brokers are down (" << m_config.brokers
                      << "); the HTTP API remains available" << std::endl;
        }
    }
#endif
}

} // namespace ods::s4::infrastructure
