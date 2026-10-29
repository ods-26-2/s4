#include "s4/infrastructure/b3_bus/kafka_event_transport.hpp"
#include "s4/infrastructure/b3_bus/envelope.hpp"

#ifdef ODS_S4_WITH_KAFKA
#include <nlohmann/json.hpp>
#endif

#include <chrono>
#include <iostream>

namespace ods::s4::infrastructure {

KafkaEventTransport::KafkaEventTransport(const KafkaEventTransportConfig& config)
    : m_config(config) {
#ifdef ODS_S4_WITH_KAFKA
    std::string errstr;
    m_conf = std::unique_ptr<RdKafka::Conf>(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    m_conf->set("bootstrap.servers", m_config.brokers, errstr);
    m_conf->set("group.id", m_config.groupId, errstr);
    m_conf->set("client.id", m_config.clientId, errstr);
    m_conf->set("auto.offset.reset", "earliest", errstr);
    m_conf->set("enable.auto.commit", "false", errstr);

    m_consumer = std::unique_ptr<RdKafka::KafkaConsumer>(RdKafka::KafkaConsumer::create(m_conf.get(), errstr));

    std::string producerErr;
    auto producerConf = std::unique_ptr<RdKafka::Conf>(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    producerConf->set("bootstrap.servers", m_config.brokers, producerErr);
    producerConf->set("client.id", m_config.clientId + "-producer", producerErr);
    m_producer = std::unique_ptr<RdKafka::Producer>(RdKafka::Producer::create(producerConf.get(), producerErr));
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
    const auto j = envelope_to_json(envelope);
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
    m_producer->poll(0);
#else
    (void)topic;
    (void)envelope;
#endif
}

void KafkaEventTransport::start() {
    if (m_running.exchange(true)) return;
#ifdef ODS_S4_WITH_KAFKA
    m_thread = std::thread([this]{ consume_loop(); });
#else
    (void)0;
#endif
}

void KafkaEventTransport::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
#ifdef ODS_S4_WITH_KAFKA
    if (m_consumer) {
        m_consumer->close();
    }
#endif
}

void KafkaEventTransport::consume_loop() {
#ifdef ODS_S4_WITH_KAFKA
    while (m_running.load()) {
        RdKafka::Message* msg = m_consumer->consume(1000);
        if (!msg) continue;
        if (msg->err() == RdKafka::ERR_NO_ERROR && msg->payload()) {
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
                m_consumer->commitSync(msg);
            } catch (const std::exception& e) {
                std::cerr << "Kafka message parse error: " << e.what() << std::endl;
                m_consumer->commitSync(msg);
            }
        }
        delete msg;
    }
#endif
}

} // namespace ods::s4::infrastructure
