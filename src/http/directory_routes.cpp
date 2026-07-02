#include "http/directory_routes.hpp"

#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"
#include "dto/message_dto.hpp"

#include <drogon/drogon.h>
#include <td/telegram/td_api.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace api = td::td_api;

namespace tgw::http {
namespace {

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;

constexpr char kBearerFilter[] = "tgw::http::BearerAuthFilter";

drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(std::move(body));
    resp->setStatusCode(code);
    return resp;
}

drogon::HttpResponsePtr serviceError(const char* code, const std::string& message,
                                     drogon::HttpStatusCode status) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    return jsonResponse(std::move(body), status);
}

drogon::HttpResponsePtr telegramError(const api::error& error, drogon::HttpStatusCode status) {
    Json::Value body;
    body["ok"] = false;
    body["error"]["code"] = "TELEGRAM_ERROR";
    body["error"]["message"] = error.message_;
    body["error"]["tdlib_code"] = error.code_;
    body["error"]["tdlib_message"] = error.message_;
    return jsonResponse(std::move(body), status);
}

bool parseId(const std::string& text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

// Detached-корутина invoke -> builder (тот же паттерн, что в message_routes: registerHandler
// не биндит корутинные лямбды).
void launchInvoke(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                  api::object_ptr<api::Function> fn, HttpCallback callback,
                  std::function<drogon::HttpResponsePtr(api::object_ptr<api::Object>)> builder) {
    [](tgw::bridge::TdBridge& td, std::int32_t cid, api::object_ptr<api::Function> f,
       HttpCallback cb, std::function<drogon::HttpResponsePtr(api::object_ptr<api::Object>)> build)
        -> drogon::AsyncTask {
        auto object = co_await td.invoke(cid, std::move(f));
        cb(build(std::move(object)));
        co_return;
    }(bridge, client_id, std::move(fn), std::move(callback), std::move(builder));
}

drogon::HttpResponsePtr chatBuilder(api::object_ptr<api::Object> obj) {
    auto chat = tgw::bridge::expect<api::chat>(std::move(obj));
    if (!chat.ok()) {
        return telegramError(*chat.error, drogon::k502BadGateway);
    }
    Json::Value body;
    body["ok"] = true;
    body["data"] = tgw::dto::toJson(*chat.value);
    return jsonResponse(std::move(body), drogon::k200OK);
}

}  // namespace

void registerDirectoryRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id) {
    auto& app = drogon::app();

    // GET /v1/resolve?username=<name> — публичный чат/юзер по @username (searchPublicChat).
    app.registerHandler(
        "/v1/resolve",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            std::string username = req->getParameter("username");
            if (!username.empty() && username.front() == '@') {
                username.erase(0, 1);
            }
            if (username.empty()) {
                cb(serviceError("VALIDATION_ERROR", "query param 'username' is required",
                                drogon::k400BadRequest));
                return;
            }
            launchInvoke(bridge, client_id, api::make_object<api::searchPublicChat>(username),
                         std::move(cb), chatBuilder);
        },
        {drogon::Get, kBearerFilter});

    // GET /v1/chats/{chatId} — карточка чата (getChat).
    app.registerHandler(
        "/v1/chats/{chatId}",
        [&bridge, client_id](const drogon::HttpRequestPtr&, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            launchInvoke(bridge, client_id, api::make_object<api::getChat>(chatId), std::move(cb),
                         chatBuilder);
        },
        {drogon::Get, kBearerFilter});

    // GET /v1/users/{userId} — карточка пользователя (getUser).
    app.registerHandler(
        "/v1/users/{userId}",
        [&bridge, client_id](const drogon::HttpRequestPtr&, HttpCallback&& cb,
                             std::string userIdStr) {
            std::int64_t userId = 0;
            if (!parseId(userIdStr, userId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid user_id", drogon::k400BadRequest));
                return;
            }
            launchInvoke(bridge, client_id, api::make_object<api::getUser>(userId), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto user = tgw::bridge::expect<api::user>(std::move(obj));
                             if (!user.ok()) {
                                 return telegramError(*user.error, drogon::k502BadGateway);
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = tgw::dto::toJson(*user.value);
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Get, kBearerFilter});

    // POST /v1/chats/join — вступить по invite-ссылке. Тело: {"invite_link": "https://t.me/+..."}.
    app.registerHandler(
        "/v1/chats/join",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            auto json = req->getJsonObject();
            if (json == nullptr || !(*json)["invite_link"].isString() ||
                (*json)["invite_link"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'invite_link' is required",
                                drogon::k400BadRequest));
                return;
            }
            launchInvoke(
                bridge, client_id,
                api::make_object<api::joinChatByInviteLink>((*json)["invite_link"].asString()),
                std::move(cb), [](api::object_ptr<api::Object> obj) {
                    if (obj != nullptr && obj->get_id() == api::error::ID) {
                        return telegramError(static_cast<api::error&>(*obj),
                                             drogon::k502BadGateway);
                    }
                    Json::Value body;
                    body["ok"] = true;
                    if (obj != nullptr && obj->get_id() == api::chatJoinResultSuccess::ID) {
                        body["data"]["result"] = "joined";
                        body["data"]["chat_id"] = std::to_string(
                            static_cast<const api::chatJoinResultSuccess&>(*obj).chat_id_);
                    } else if (obj != nullptr &&
                               obj->get_id() == api::chatJoinResultRequestSent::ID) {
                        body["data"]["result"] = "request_sent";  // чат с апрувом заявок
                    } else {
                        body["data"]["result"] = "declined";
                    }
                    return jsonResponse(std::move(body), drogon::k200OK);
                });
        },
        {drogon::Post, kBearerFilter});

    // GET /v1/chats/{chatId}/members?query=&limit= — участники (searchChatMembers: работает
    // для всех типов чатов; пустой query = все, limit<=200).
    app.registerHandler(
        "/v1/chats/{chatId}/members",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            std::int32_t limit = 50;
            const std::string limit_str = req->getParameter("limit");
            if (!limit_str.empty()) {
                limit = std::max(1, std::min(200, std::atoi(limit_str.c_str())));
            }
            auto fn = api::make_object<api::searchChatMembers>();
            fn->chat_id_ = chatId;
            fn->query_ = req->getParameter("query");
            fn->limit_ = limit;
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto members = tgw::bridge::expect<api::chatMembers>(std::move(obj));
                             if (!members.ok()) {
                                 return telegramError(*members.error, drogon::k502BadGateway);
                             }
                             Json::Value list(Json::arrayValue);
                             for (const auto& m : members.value->members_) {
                                 if (m != nullptr) {
                                     list.append(tgw::dto::toJson(*m));
                                 }
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = list;
                             body["meta"]["total_count"] = members.value->total_count_;
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Get, kBearerFilter});

    // GET /v1/contacts — id контактов (getContacts). Карточки — по /v1/users/{id}.
    app.registerHandler("/v1/contacts",
                        [&bridge, client_id](const drogon::HttpRequestPtr&, HttpCallback&& cb) {
                            launchInvoke(
                                bridge, client_id, api::make_object<api::getContacts>(),
                                std::move(cb), [](api::object_ptr<api::Object> obj) {
                                    auto users = tgw::bridge::expect<api::users>(std::move(obj));
                                    if (!users.ok()) {
                                        return telegramError(*users.error, drogon::k502BadGateway);
                                    }
                                    Json::Value ids(Json::arrayValue);
                                    for (const auto id : users.value->user_ids_) {
                                        ids.append(std::to_string(id));
                                    }
                                    Json::Value body;
                                    body["ok"] = true;
                                    body["data"]["user_ids"] = ids;
                                    body["data"]["total_count"] = users.value->total_count_;
                                    return jsonResponse(std::move(body), drogon::k200OK);
                                });
                        },
                        {drogon::Get, kBearerFilter});
}

}  // namespace tgw::http
