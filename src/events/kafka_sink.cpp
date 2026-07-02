#include "events/kafka_sink.hpp"

#include "util/metrics.hpp"

#include <trantor/utils/Logger.h>

#include <atomic>
#include <librdkafka/rdkafkacpp.h>
#include <string>

namespace tgw::events {
namespace {

// Delivery-report: успех/провал доставки после ретраев librdkafka.
class CountingDeliveryCb final : public RdKafka::DeliveryReportCb {
   public:
    void dr_cb(RdKafka::Message& message) override {
        auto& c = tgw::metrics::Counters::instance();
        if (message.err() == RdKafka::ERR_NO_ERROR) {
            c.kafka_produced_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            c.kafka_failed_total.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR << "kafka delivery failed: " << message.errstr();
        }
    }
};

}  // namespace

std::unique_ptr<KafkaSink> KafkaSink::create(const KafkaConfig& config) {
    if (!config.enabled()) {
        return nullptr;
    }
    std::string err;
    std::unique_ptr<RdKafka::Conf> conf(RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));
    auto delivery_cb = std::make_unique<CountingDeliveryCb>();
    if (conf->set("bootstrap.servers", config.brokers, err) != RdKafka::Conf::CONF_OK ||
        conf->set("client.id", config.client_id, err) != RdKafka::Conf::CONF_OK ||
        conf->set("dr_cb", delivery_cb.get(), err) != RdKafka::Conf::CONF_OK) {
        LOG_ERROR << "kafka config error: " << err;
        return nullptr;
    }

    std::unique_ptr<RdKafka::Producer> producer(RdKafka::Producer::create(conf.get(), err));
    if (producer == nullptr) {
        LOG_ERROR << "kafka producer create failed: " << err;
        return nullptr;
    }

    auto sink = std::unique_ptr<KafkaSink>(new KafkaSink());
    sink->delivery_cb_ = std::move(delivery_cb);
    sink->producer_ = std::move(producer);
    sink->topic_ = config.topic;
    LOG_INFO << "kafka sink enabled: brokers=" << config.brokers << " topic=" << config.topic;
    return sink;
}

KafkaSink::~KafkaSink() = default;

void KafkaSink::produce(const std::string& key, const std::string& payload) {
    // RD_KAFKA_MSG_F_COPY: librdkafka копирует payload — наши буферы ему не принадлежат.
    const RdKafka::ErrorCode err = producer_->produce(
        topic_, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
        const_cast<char*>(payload.data()), payload.size(), key.data(), key.size(),
        /*timestamp=*/0, /*headers=*/nullptr, /*opaque=*/nullptr);
    if (err != RdKafka::ERR_NO_ERROR) {
        // Очередь полна / брокер давно недоступен: НЕ блокируемся (поток-приёмник TDLib),
        // событие теряется — фиксируем метрикой; лог не на каждый drop, чтобы не зашуметь.
        static std::atomic<std::uint64_t> log_gate{0};
        const auto dropped = tgw::metrics::Counters::instance().kafka_dropped_total.fetch_add(
                                 1, std::memory_order_relaxed) +
                             1;
        if (log_gate.fetch_add(1, std::memory_order_relaxed) % 1000 == 0) {
            LOG_ERROR << "kafka produce dropped (total " << dropped
                      << "): " << RdKafka::err2str(err);
        }
    }
    producer_->poll(0);  // заодно обслуживаем delivery-report'ы
}

void KafkaSink::poll() {
    producer_->poll(0);
}

void KafkaSink::flush(std::chrono::milliseconds timeout) {
    const RdKafka::ErrorCode err = producer_->flush(static_cast<int>(timeout.count()));
    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_WARN << "kafka flush incomplete: " << RdKafka::err2str(err);
    }
}

}  // namespace tgw::events
