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
    const auto raw = store_.load();
    std::vector<Webhook> parsed;
    if (!raw.has_value()) {
        LOG_WARN << "webhook registry: no stored state, starting empty";
    } else {
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const bool parsed_ok =
            reader->parse(raw->data(), raw->data() + raw->size(), &root, &errs);
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
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::find_if(hooks_.begin(), hooks_.end(), [&id](const Webhook& h) { return h.id == id; });
    if (it != hooks_.end()) {
        // Дубль url -> тот же id: обновляем запись на месте, не плодим дубликаты.
        it->url = url;
        it->secret = secret;
        it->active = active;
    } else {
        hooks_.push_back(Webhook{id, url, secret, active});
    }
    persist();
    return id;
}

bool WebhookRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it =
        std::find_if(hooks_.begin(), hooks_.end(), [&id](const Webhook& h) { return h.id == id; });
    if (it == hooks_.end()) {
        return false;
    }
    hooks_.erase(it);
    persist();
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

void WebhookRegistry::persist() {
    Json::Value arr(Json::arrayValue);
    for (const auto& hook : hooks_) {
        arr.append(webhookToJson(hook));
    }
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string json = Json::writeString(builder, arr);
    if (!store_.save(json)) {
        LOG_ERROR << "webhook registry: failed to persist " << hooks_.size() << " webhook(s)";
    }
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

   private:
    tgw::util::S3Client client_;
};

}  // namespace

std::unique_ptr<IWebhookStore> makeS3WebhookStore(tgw::util::S3Config config) {
    return std::make_unique<S3WebhookStore>(std::move(config));
}

}  // namespace tgw::webhook
