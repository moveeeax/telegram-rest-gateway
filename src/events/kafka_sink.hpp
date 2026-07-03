#pragma once

#include <chrono>
#include <memory>
#include <string>

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

// Продюсер событий в Kafka. Правила (§дизайн-решения):
//  - produce() НЕ блокирует: вызывается из потока-приёмника TDLib, которому нельзя вставать
//    (он же обслуживает ответы HTTP и WS). Переполнение внутренней очереди librdkafka =>
//    drop + tgw_kafka_dropped_total (сервис первичен, Kafka — дополнительный канал).
//  - Доставка at-least-once (ретраи librdkafka) — консьюмер дедуплицирует по (session_id, seq)
//    из тела события; ключ сообщения = "<session_id>:<chat_id>" (порядок в рамках чата).
//  - poll() обслуживает delivery-report'ы (счётчики produced/failed) — дёргается таймером.
class KafkaSink {
   public:
    // nullptr — если выключено (brokers пуст) или librdkafka не смог создать продюсера.
    static std::unique_ptr<KafkaSink> create(const KafkaConfig& config);
    ~KafkaSink();

    KafkaSink(const KafkaSink&) = delete;
    KafkaSink& operator=(const KafkaSink&) = delete;

    void produce(const std::string& key, const std::string& payload);
    void poll();  // неблокирующий сервис колбэков
    void flush(std::chrono::milliseconds timeout);  // graceful shutdown

   private:
    KafkaSink() = default;

    std::unique_ptr<RdKafka::DeliveryReportCb> delivery_cb_;
    std::unique_ptr<RdKafka::Producer> producer_;
    std::string topic_;
};

}  // namespace tgw::events
