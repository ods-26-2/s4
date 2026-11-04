#pragma once

#ifdef ODS_S4_WITH_KAFKA
#include <librdkafka/rdkafkacpp.h>
#endif

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "s4/infrastructure/b3_bus/event_transport.hpp"
#include "s4/infrastructure/b3_bus/kafka_config.hpp"

namespace ods::s4::infrastructure {

class KafkaEventTransport : public IEventTransport {
public:
    explicit KafkaEventTransport(const KafkaEventTransportConfig& config);
    ~KafkaEventTransport() override;

    void subscribe(const std::string& topic, MessageHandler handler) override;
    void publish(const std::string& topic, const B3Envelope& envelope) override;
    void start() override;
    void stop() override;

private:
    void consume_loop();

    KafkaEventTransportConfig m_config;
    std::vector<std::pair<std::string, MessageHandler>> m_subscriptions;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::mutex m_mutex;

#ifdef ODS_S4_WITH_KAFKA
    std::unique_ptr<RdKafka::Conf> m_conf;
    std::unique_ptr<RdKafka::KafkaConsumer> m_consumer;
    std::unique_ptr<RdKafka::Producer> m_producer;
#endif
};

} // namespace ods::s4::infrastructure
