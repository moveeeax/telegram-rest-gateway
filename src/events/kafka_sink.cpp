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
    sink->worker_ = std::thread([raw = sink.get()] { raw->workerLoop(); });
    LOG_INFO << "kafka sink enabled: brokers=" << config.brokers << " topic=" << config.topic;
    return sink;
}

KafkaSink::~KafkaSink() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

// Только enqueue (вызывается потоком-приёмником TDLib): librdkafka здесь НЕ трогаем.
void KafkaSink::produce(const std::string& key, const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueue) {
            // Воркер/брокер не успевает — дропаем, чтобы не жрать память (сервис первичен).
            static std::atomic<std::uint64_t> log_gate{0};
            const auto dropped = tgw::metrics::Counters::instance().kafka_dropped_total.fetch_add(
                                     1, std::memory_order_relaxed) +
                                 1;
            if (log_gate.fetch_add(1, std::memory_order_relaxed) % 1000 == 0) {
                LOG_ERROR << "kafka queue full, dropped (total " << dropped << ")";
            }
            return;
        }
        queue_.emplace_back(key, payload);
    }
    cv_.notify_one();
}

// Единственный поток, трогающий librdkafka: produce()+poll() у продюсера.
void KafkaSink::workerLoop() {
    std::deque<std::pair<std::string, std::string>> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Пока есть неподтверждённые сообщения (outq_len > 0), спим не дольше 100мс, чтобы на
            // каждом пробуждении звать poll(0) и разгребать delivery-report'ы последнего батча:
            // иначе они висли бы до следующего produce() — метрики produced/failed врут, буферы
            // RK_MSG_COPY держатся в памяти. Пустая очередь и ноль outstanding — ждём без таймаута
            // (нечего поллить, спим до produce()). outq_len() трогает librdkafka, но это worker-
            // тред под mutex_ — инвариант «librdkafka только из воркера» соблюдён.
            if (producer_->outq_len() > 0) {
                cv_.wait_for(lock, std::chrono::milliseconds(100),
                             [this] { return stop_ || !queue_.empty(); });
            } else {
                cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            }
            batch.swap(queue_);
            if (batch.empty() && stop_) {
                return;
            }
        }
        for (const auto& [key, payload] : batch) {
            const RdKafka::ErrorCode err = producer_->produce(
                topic_, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
                const_cast<char*>(payload.data()), payload.size(), key.data(), key.size(),
                /*timestamp=*/0, /*headers=*/nullptr, /*opaque=*/nullptr);
            if (err != RdKafka::ERR_NO_ERROR) {
                static std::atomic<std::uint64_t> log_gate{0};
                const auto dropped =
                    tgw::metrics::Counters::instance().kafka_dropped_total.fetch_add(
                        1, std::memory_order_relaxed) +
                    1;
                if (log_gate.fetch_add(1, std::memory_order_relaxed) % 1000 == 0) {
                    LOG_ERROR << "kafka produce dropped (total " << dropped
                              << "): " << RdKafka::err2str(err);
                }
            }
        }
        batch.clear();
        producer_->poll(0);  // delivery-report'ы — на этом же потоке
    }
}

void KafkaSink::flush(std::chrono::milliseconds timeout) {
    // Останавливаем воркер (он дольше не трогает продюсер), затем flush из ЭТОГО потока —
    // единоличный доступ сохраняется (воркер уже присоединён).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    const RdKafka::ErrorCode err = producer_->flush(static_cast<int>(timeout.count()));
    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_WARN << "kafka flush incomplete: " << RdKafka::err2str(err);
    }
}

}  // namespace tgw::events
