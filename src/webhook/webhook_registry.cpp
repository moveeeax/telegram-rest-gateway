#include "webhook/webhook_registry.hpp"

#include "util/aws_sigv4.hpp"

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tgw::webhook {
namespace {

// Сериализует один Webhook в JSON-объект {id,url,secret,active}.
Json::Value webhookToJson(const Webhook& hook) {
    Json::Value item;
    item["id"] = hook.id;
    item["url"] = hook.url;
    item["secret"] = hook.secret;
    item["active"] = hook.active;
    return item;
}

// Парсит один элемент массива реестра. false — элемент не объект/без id, пропускаем его
// (не валим весь реестр из-за одной битой записи).
bool jsonToWebhook(const Json::Value& item, Webhook& out) {
    if (!item.isObject() || !item["id"].isString() || item["id"].asString().empty()) {
        return false;
    }
    out.id = item["id"].asString();
    out.url = item["url"].isString() ? item["url"].asString() : "";
    out.secret = item["secret"].isString() ? item["secret"].asString() : "";
    out.active = item["active"].isBool() ? item["active"].asBool() : true;
    return true;
}

}  // namespace

WebhookRegistry::WebhookRegistry(IWebhookStore& store) : store_(store) {}

void WebhookRegistry::loadFromStore() {
    // Сеть — НИКОГДА под mutex_ (см. persist()): store_.load() у прод-реализации тоже блокирующий
    // HTTP GET, зовём его до захвата лока; сам лок берём только на короткое присваивание hooks_.
    const auto raw = store_.load();
    std::vector<Webhook> parsed;
    if (!raw.has_value()) {
        LOG_WARN << "webhook registry: no stored state, starting empty";
    } else {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const bool parsed_ok = reader->parse(raw->data(), raw->data() + raw->size(), &root, &errs);
        if (!parsed_ok || !root.isArray()) {
            LOG_WARN << "webhook registry: failed to parse stored JSON, starting empty"
                     << (parsed_ok ? "" : (": " + errs));
        } else {
            parsed.reserve(root.size());
            for (const auto& item : root) {
                Webhook hook;
                if (jsonToWebhook(item, hook)) {
                    parsed.push_back(std::move(hook));
                }
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_ = std::move(parsed);
}

std::string WebhookRegistry::add(const std::string& url, const std::string& secret, bool active) {
    // Детерминированный id: первые 16 hex-символов sha256(url) (8 байт) — стабилен между
    // рестартами без хранения отдельного счётчика/генератора uuid.
    const std::string id = tgw::util::sha256Hex(url).substr(0, 16);
    std::string snapshot_json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(hooks_.begin(), hooks_.end(),
                                     [&id](const Webhook& h) { return h.id == id; });
        if (it != hooks_.end()) {
            // Дубль url -> тот же id: обновляем запись на месте, не плодим дубликаты.
            it->url = url;
            it->secret = secret;
            it->active = active;
        } else {
            hooks_.push_back(Webhook{id, url, secret, active});
        }
        snapshot_json = serializeLocked();
    }  // лок отпущен ДО сетевого save() — см. persist().
    persist(snapshot_json);
    return id;
}

bool WebhookRegistry::remove(const std::string& id) {
    std::string snapshot_json;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(hooks_.begin(), hooks_.end(),
                                     [&id](const Webhook& h) { return h.id == id; });
        if (it == hooks_.end()) {
            return false;
        }
        hooks_.erase(it);
        snapshot_json = serializeLocked();
    }  // лок отпущен ДО сетевого save() — см. persist().
    persist(snapshot_json);
    return true;
}

std::vector<Webhook> WebhookRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hooks_;
}

std::vector<Webhook> WebhookRegistry::activeSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Webhook> out;
    out.reserve(hooks_.size());
    for (const auto& hook : hooks_) {
        if (hook.active) {
            out.push_back(hook);
        }
    }
    return out;
}

// Сериализует hooks_ в JSON-массив. ТОЛЬКО из-под mutex_ (см. add/remove) — сама сеть не трогает,
// значит безопасна под локом (в отличие от persist() ниже).
std::string WebhookRegistry::serializeLocked() const {
    Json::Value arr(Json::arrayValue);
    for (const auto& hook : hooks_) {
        arr.append(webhookToJson(hook));
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, arr);
}

// Сеть — НИКОГДА под mutex_. store_.save() у прод-реализации (S3WebhookStore) делает синхронный
// HTTP PUT (до S3Client::kSendHangTimeout = 35s, см. util/s3_client.hpp) — вызов под mutex_ держал
// бы лок всё это время и стопорил бы activeSnapshot() диспетчера (горячий путь доставки
// вебхуков) на время одной админской add/remove при медленном/недоступном S3. Поэтому вызывающий
// (add/remove) обязан собрать snapshot_json через serializeLocked() ПОД локом, отпустить лок и
// только потом позвать persist(snapshot_json).
//
// Но отпустить лок мало: add/remove — это обычные (НЕ корутинные) HTTP-хендлеры POST /v1/webhooks
// и DELETE /v1/webhooks/{id}, исполняются НА IO-петле Drogon. Синхронный store_.save() у S3
// подвесил бы саму петлю на десятки секунд, остановив весь остальной HTTP-трафик на ней. Поэтому
// для async-стора (isAsync()==true) саму заливку уносим в detached-поток — тот же приём, что
// auth/s3_session.cpp (startS3Sync: "S3Client::send блокирующий, поэтому саму заливку уносим в
// отдельный поток, чтобы не застопорить loop"). persist() возвращает управление сразу, реальная
// сетевая запись идёт в фоне; add/remove остаются неблокирующими для HTTP-хендлера.
//
// Компромисс fire-and-forget осознан: результат detached-записи нельзя обработать в вызывающем
// коде синхронно, только залогировать (LOG_INFO/LOG_ERROR). Это приемлемо: webhooks.json — не
// критичный для целостности объект уровня td.binlog; snapshot_json — ПОЛНЫЙ снимок реестра (не
// дельта), поэтому потеря одной фоновой записи между рестартами не фатальна — следующая мутация
// перезапишет актуальное состояние целиком. Потоки детачим без ограничения (как startS3Sync):
// админские add/remove редки, осознанный trade-off уже принят в проекте для похожего кейса.
//
// Синхронный стор (FakeStore в тестах, isAsync()==false) сохраняем прямо здесь: сети нет,
// блокировать петлю нечем, а тесты рассчитывают увидеть результат сразу после add/remove.
void WebhookRegistry::persist(const std::string& snapshot_json) {
    if (!store_.isAsync()) {
        if (!store_.save(snapshot_json)) {
            LOG_ERROR << "webhook registry: failed to persist webhook registry";
        }
        return;
    }
    // store_ живёт весь срок жизни процесса (WebhookRegistry — долгоживущий синглтон сервиса),
    // поэтому захват его по указателю в detached-поток безопасен. snapshot_json копируем в поток.
    IWebhookStore* const store = &store_;
    std::thread([store, snapshot_json]() {
        if (store->save(snapshot_json)) {
            LOG_INFO << "webhook registry: persisted (" << snapshot_json.size() << " bytes)";
        } else {
            LOG_ERROR << "webhook registry: failed to persist webhook registry";
        }
    }).detach();
}

namespace {

// S3-обёртка IWebhookStore поверх util::S3Client: единственная реализация персиста для прод-кода
// (тесты используют FakeStore из tests/webhook_registry_test.cpp).
class S3WebhookStore final : public IWebhookStore {
   public:
    explicit S3WebhookStore(tgw::util::S3Config config) : client_(std::move(config)) {}

    std::optional<std::string> load() override {
        const auto result = client_.get();
        if (result.notFound()) {
            return std::nullopt;
        }
        if (!result.ok()) {
            LOG_ERROR << "webhook registry: s3 load failed: http=" << result.http_status << " "
                      << result.error;
            return std::nullopt;
        }
        return result.body;
    }

    bool save(const std::string& json) override {
        const auto result = client_.put(json);
        if (!result.ok()) {
            LOG_ERROR << "webhook registry: s3 save failed: http=" << result.http_status << " "
                      << result.error;
        }
        return result.ok();
    }

    // Прод-стор: put() блокирующий HTTP — persist() уносит save() в detached-поток, чтобы не
    // застопорить IO-петлю Drogon (см. WebhookRegistry::persist).
    bool isAsync() const override { return true; }

   private:
    tgw::util::S3Client client_;
};

}  // namespace

std::unique_ptr<IWebhookStore> makeS3WebhookStore(tgw::util::S3Config config) {
    return std::make_unique<S3WebhookStore>(std::move(config));
}

}  // namespace tgw::webhook
