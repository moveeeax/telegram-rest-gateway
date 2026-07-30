#pragma once

#include "util/s3_client.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tgw::webhook {

// Один вебхук: URL получателя mention/reply-событий, секрет для подписи payload'а (HMAC — вне
// этого файла) и флаг активности. Неактивные остаются в реестре (пауза без удаления), но не
// попадают в activeSnapshot — тем самым не участвуют в рассылке.
struct Webhook {
    std::string id;
    std::string url;
    std::string secret;
    bool active = true;
};

// Абстракция персиста ради тестируемости WebhookRegistry без сети (FakeStore в тестах).
// Реальная реализация — S3WebhookStore поверх util::S3Client (см. makeS3WebhookStore ниже).
class IWebhookStore {
   public:
    virtual ~IWebhookStore() = default;

    // Сырой JSON реестра или nullopt (объекта ещё нет / транспортная ошибка).
    virtual std::optional<std::string> load() = 0;
    // true — успешно сохранено.
    virtual bool save(const std::string& json) = 0;
};

// In-memory реестр вебхуков с персистом в store_ на каждую мутацию (add/remove). Читается
// диспетчером обновлений (activeSnapshot, другой поток) и пишется REST-роутами (add/remove) —
// все методы под mutex_, list/activeSnapshot возвращают КОПИЮ под локом.
class WebhookRegistry {
   public:
    explicit WebhookRegistry(IWebhookStore& store);

    // Восстанавливает состояние из store_ (вызывается на старте сервиса). Отсутствие объекта
    // в сторе или ошибка парсинга JSON не фатальны — реестр остаётся пустым, в лог уходит
    // предупреждение.
    void loadFromStore();

    // Добавляет вебхук и персистит реестр. id — детерминированный: hex(sha256(url))[0:16] через
    // tgw::util::sha256Hex (стабилен между рестартами; дубль url → тот же id, запись
    // обновляется на месте, а не дублируется). Возвращает id.
    std::string add(const std::string& url, const std::string& secret, bool active);
    // true — запись с таким id существовала и была удалена (реестр персистнут); false — id
    // не найден.
    bool remove(const std::string& id);

    std::vector<Webhook> list() const;
    // Только active == true.
    std::vector<Webhook> activeSnapshot() const;

   private:
    mutable std::mutex mutex_;
    std::vector<Webhook> hooks_;
    IWebhookStore& store_;

    // Сериализует hooks_ в JSON и сохраняет через store_.save(). Вызывается ТОЛЬКО из-под
    // mutex_ (add/remove) — сам лок не берёт.
    void persist();
};

// S3-реализация IWebhookStore: хранит реестр по ключу из config (тот же объект/бакет, что и
// остальной state сервиса). load(): 404 -> nullopt, 2xx -> тело, иная ошибка -> nullopt + лог.
// save(): put(json), true при 2xx.
std::unique_ptr<IWebhookStore> makeS3WebhookStore(tgw::util::S3Config config);

}  // namespace tgw::webhook
