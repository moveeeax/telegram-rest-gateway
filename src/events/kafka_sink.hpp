#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace RdKafka {
class Producer;
class DeliveryReportCb;
}  // namespace RdKafka

namespace tgw::events {

// Конфиг Kafka-канала событий. brokers пуст — фича выключена (как S3Config).
struct KafkaConfig {
    std::string brokers;  // bootstrap.servers, напр. "redpanda:9092"
    std::string topic = "tgw.updates";
    std::string client_id = "telegram-rest-gateway";

    bool enabled() const { return !brokers.empty(); }
};

// Продюсер событий в Kafka. АРХИТЕКТУРА (после инцидента heap-corruption на amd64):
// librdkafka трогает РОВНО ОДИН выделенный поток-воркер. produce() лишь кладёт (key,payload)
// в очередь под мьютексом и будит воркер — вызывается из потока-приёмника TDLib, который
// НИКОГДА не заходит в librdkafka (у него своя тяжёлая аллокация; смешение с librdkafka на
// одном потоке приводило к порче кучи). Воркер делает produce()+poll() у продюсера.
//  - produce() НЕ блокирует; переполнение очереди => drop + tgw_kafka_dropped_total.
//  - Доставка at-least-once; ключ = "<session_id>:<chat_id>" (порядок в рамках чата).
class KafkaSink {
   public:
    // nullptr — если выключено (brokers пуст) или librdkafka не смог создать продюсера.
    static std::unique_ptr<KafkaSink> create(const KafkaConfig& config);
    ~KafkaSink();

    KafkaSink(const KafkaSink&) = delete;
    KafkaSink& operator=(const KafkaSink&) = delete;

    void produce(const std::string& key, const std::string& payload);  // enqueue, неблокирующий
    void flush(std::chrono::milliseconds timeout);  // graceful shutdown: слить очередь + flush

   private:
    KafkaSink() = default;
    void workerLoop();

    std::unique_ptr<RdKafka::DeliveryReportCb> delivery_cb_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::string topic_;

    // Очередь (key,payload) от потока-приёмника к воркеру.
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> queue_;
    bool stop_ = false;
    static constexpr std::size_t kMaxQueue = 20000;  // защита памяти при затыке брокера
    std::thread worker_;
};

}  // namespace tgw::events
