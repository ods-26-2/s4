#pragma once

#ifdef ODS_S4_WITH_KAFKA
#include <librdkafka/rdkafkacpp.h>
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
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
    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_allBrokersDownLogged{false};
    std::thread m_thread;
    std::mutex m_mutex;

#ifdef ODS_S4_WITH_KAFKA
    // Callback de entrega do produtor. Declarado antes do produtor para
    // sobreviver a ele (membros sao destruidos na ordem inversa).
    class DeliveryReporter : public RdKafka::DeliveryReportCb {
    public:
        void dr_cb(RdKafka::Message& message) override;
        std::atomic<std::uint64_t> failures{0};
    };

    DeliveryReporter m_deliveryReporter;
    std::unique_ptr<RdKafka::Conf> m_conf;
    std::unique_ptr<RdKafka::KafkaConsumer> m_consumer;
    std::unique_ptr<RdKafka::Producer> m_producer;
#endif
};

} // namespace ods::s4::infrastructure
