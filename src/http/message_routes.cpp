#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"
#include "dto/message_dto.hpp"
#include "http/routes.hpp"

#include <drogon/drogon.h>
#include <td/telegram/td_api.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace api = td::td_api;

namespace tgw::http {
namespace {

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
using ResponseBuilder = std::function<drogon::HttpResponsePtr(api::object_ptr<api::Object>)>;

constexpr char kBearerFilter[] = "tgw::http::BearerAuthFilter";

drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    resp->setStatusCode(code);
    return resp;
}

drogon::HttpResponsePtr serviceError(const std::string& code, const std::string& message,
                                     drogon::HttpStatusCode http) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    return jsonResponse(std::move(body), http);
}

drogon::HttpResponsePtr telegramError(const api::error& error, drogon::HttpStatusCode http) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "TELEGRAM_ERROR";
    body["error"]["message"] = error.message_;
    body["error"]["tdlib_code"] = error.code_;
    body["error"]["tdlib_message"] = error.message_;
    return jsonResponse(std::move(body), http);
}

bool parseId(const std::string& str, std::int64_t& out) {
    try {
        std::size_t pos = 0;
        out = std::stoll(str, &pos);
        return pos == str.size();
    } catch (...) {
        return false;
    }
}

std::int32_t queryInt(const drogon::HttpRequestPtr& req, const char* name, std::int32_t def,
                      std::int32_t max) {
    const std::string& value = req->getParameter(name);
    if (value.empty()) {
        return def;
    }
    try {
        const int parsed = std::stoi(value);
        if (parsed < 1) {
            return def;
        }
        return parsed > max ? max : parsed;
    } catch (...) {
        return def;
    }
}

// Общий запуск detached-корутины: co_await ответа TDLib, затем builder строит HTTP-ответ.
void launchInvoke(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                  api::object_ptr<api::Function> fn, HttpCallback callback, ResponseBuilder build) {
    [](tgw::bridge::TdBridge& td, std::int32_t cid, api::object_ptr<api::Function> f,
       HttpCallback cb, ResponseBuilder builder) -> drogon::AsyncTask {
        auto object = co_await td.invoke(cid, std::move(f));
        cb(builder(std::move(object)));
        co_return;
    }(bridge, client_id, std::move(fn), std::move(callback), std::move(build));
}

api::object_ptr<api::Function> makeGetChats(std::int32_t limit) {
    auto fn = api::make_object<api::getChats>();
    fn->chat_list_ = api::make_object<api::chatListMain>();
    fn->limit_ = limit;
    return fn;
}

api::object_ptr<api::Function> makeLoadChats(std::int32_t limit) {
    auto fn = api::make_object<api::loadChats>();
    fn->chat_list_ = api::make_object<api::chatListMain>();
    fn->limit_ = limit;
    return fn;
}

}  // namespace

void registerMessageRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id) {
    auto& app = drogon::app();

    // GET /v1/chats — best-effort loadChats + getChats, затем getChat на каждый id (§8.2.4).
    // MVP: возможен частичный список сразу после старта (чаты подгружаются апдейтами) — норма.
    app.registerHandler(
        "/v1/chats",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            const std::int32_t limit = queryInt(req, "limit", 20, 100);
            [](tgw::bridge::TdBridge& td, std::int32_t cid, std::int32_t lim,
               HttpCallback callback) -> drogon::AsyncTask {
                co_await td.invoke(cid, makeLoadChats(lim));  // прогрев, результат игнорируем
                auto chatsObj = co_await td.invoke(cid, makeGetChats(lim));
                auto chats = tgw::bridge::expect<api::chats>(std::move(chatsObj));
                if (!chats.ok()) {
                    callback(telegramError(*chats.error, drogon::k502BadGateway));
                    co_return;
                }
                Json::Value arr(Json::arrayValue);
                for (const std::int64_t id : chats.value->chat_ids_) {
                    auto chatObj = co_await td.invoke(cid, api::make_object<api::getChat>(id));
                    auto chat = tgw::bridge::expect<api::chat>(std::move(chatObj));
                    if (chat.ok()) {
                        arr.append(tgw::dto::toJson(*chat.value));
                    }
                }
                Json::Value body;
                body["ok"] = true;
                body["data"] = arr;
                callback(jsonResponse(std::move(body), drogon::k200OK));
                co_return;
            }(bridge, client_id, limit, std::move(cb));
        },
        {drogon::Get, kBearerFilter});

    // GET /v1/chats/{chatId}/messages — история (§8.2.5). from_id=0 => от последнего.
    app.registerHandler(
        "/v1/chats/{chatId}/messages",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            std::int64_t fromId = 0;
            parseId(req->getParameter("from_id"), fromId);  // пусто/битое => 0 (от последнего)
            const std::int32_t limit = queryInt(req, "limit", 30, 100);

            auto fn = api::make_object<api::getChatHistory>();
            fn->chat_id_ = chatId;
            fn->from_message_id_ = fromId;
            fn->offset_ = 0;
            fn->limit_ = limit;
            fn->only_local_ = false;

            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto messages = tgw::bridge::expect<api::messages>(std::move(obj));
                             if (!messages.ok()) {
                                 return telegramError(*messages.error, drogon::k502BadGateway);
                             }
                             Json::Value arr(Json::arrayValue);
                             std::string oldest;
                             for (const auto& msg : messages.value->messages_) {
                                 if (msg != nullptr) {
                                     arr.append(tgw::dto::toJson(*msg));
                                     oldest = std::to_string(msg->id_);
                                 }
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = arr;
                             body["meta"]["next_cursor"] =
                                 oldest.empty() ? Json::Value() : Json::Value(oldest);
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Get, kBearerFilter});

    // POST /v1/chats/{chatId}/messages — отправка текста (§8.2.6): 202 + temp id, финал по WS.
    app.registerHandler(
        "/v1/chats/{chatId}/messages",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            auto json = req->getJsonObject();
            if (json == nullptr || !(*json)["text"].isString() ||
                (*json)["text"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'text' is required",
                                drogon::k400BadRequest));
                return;
            }
            auto content = api::make_object<api::inputMessageText>();
            content->text_ = api::make_object<api::formattedText>();
            content->text_->text_ = (*json)["text"].asString();

            auto fn = api::make_object<api::sendMessage>();
            fn->chat_id_ = chatId;
            fn->input_message_content_ = std::move(content);

            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto message = tgw::bridge::expect<api::message>(std::move(obj));
                             if (!message.ok()) {
                                 return telegramError(*message.error, drogon::k502BadGateway);
                             }
                             Json::Value data;
                             data["temporary_message_id"] = std::to_string(message.value->id_);
                             data["chat_id"] = std::to_string(message.value->chat_id_);
                             data["sending_state"] = "pending";
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = data;
                             return jsonResponse(std::move(body), drogon::k202Accepted);
                         });
        },
        {drogon::Post, kBearerFilter});

    // POST /v1/chats/{chatId}/messages/read — отметить прочитанным (§8.2.8).
    app.registerHandler(
        "/v1/chats/{chatId}/messages/read",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            auto json = req->getJsonObject();
            if (json == nullptr || !(*json)["message_ids"].isArray()) {
                cb(serviceError("VALIDATION_ERROR", "field 'message_ids' (array) is required",
                                drogon::k400BadRequest));
                return;
            }
            std::vector<std::int64_t> ids;
            for (const auto& item : (*json)["message_ids"]) {
                std::int64_t id = 0;
                if (item.isString() && parseId(item.asString(), id)) {
                    ids.push_back(id);
                }
            }

            auto fn = api::make_object<api::viewMessages>();
            fn->chat_id_ = chatId;
            fn->message_ids_ = std::move(ids);
            fn->force_read_ = true;

            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             if (obj != nullptr && obj->get_id() == api::error::ID) {
                                 return telegramError(static_cast<api::error&>(*obj),
                                                      drogon::k502BadGateway);
                             }
                             Json::Value body;
                             body["ok"] = true;
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Post, kBearerFilter});
}

}  // namespace tgw::http
