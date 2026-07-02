#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"
#include "dto/file_dto.hpp"
#include "dto/message_dto.hpp"
#include "http/routes.hpp"

#include <drogon/drogon.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace api = td::td_api;

namespace tgw::http {
namespace {

using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
using ResponseBuilder = std::function<drogon::HttpResponsePtr(api::object_ptr<api::Object>)>;

constexpr char kBearerFilter[] = "tgw::http::BearerAuthFilter";

// formattedText из text (+опц. parse_mode "markdown"|"html"). parseTextEntities — offline-метод
// TDLib: исполняется синхронно через ClientManager::execute, event-loop не задействован.
// При ошибке разметки/неизвестном режиме возвращает nullptr и заполняет err.
api::object_ptr<api::formattedText> makeFormattedText(const std::string& text,
                                                      const std::string& parse_mode,
                                                      std::string& err) {
    if (parse_mode.empty()) {
        auto formatted = api::make_object<api::formattedText>();
        formatted->text_ = text;
        return formatted;
    }
    auto parse = api::make_object<api::parseTextEntities>();
    parse->text_ = text;
    if (parse_mode == "markdown") {
        parse->parse_mode_ = api::make_object<api::textParseModeMarkdown>(2);
    } else if (parse_mode == "html") {
        parse->parse_mode_ = api::make_object<api::textParseModeHTML>();
    } else {
        err = "parse_mode must be 'markdown' or 'html'";
        return nullptr;
    }
    auto result = td::ClientManager::execute(std::move(parse));
    if (result == nullptr || result->get_id() == api::error::ID) {
        err = (result != nullptr) ? static_cast<api::error&>(*result).message_
                                  : "parseTextEntities failed";
        return nullptr;
    }
    return api::move_object_as<api::formattedText>(result);
}

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

void registerMessageRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                           const std::string& upload_dir) {
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
            std::string parse_err;
            auto formatted = makeFormattedText(
                (*json)["text"].asString(),
                (*json)["parse_mode"].isString() ? (*json)["parse_mode"].asString() : "",
                parse_err);
            if (formatted == nullptr) {
                cb(serviceError("VALIDATION_ERROR", parse_err, drogon::k400BadRequest));
                return;
            }
            auto content = api::make_object<api::inputMessageText>();
            content->text_ = std::move(formatted);

            auto fn = api::make_object<api::sendMessage>();
            fn->chat_id_ = chatId;
            // Опциональный ответ на сообщение (reply): id строкой, как все id наружу.
            std::int64_t replyTo = 0;
            if ((*json)["reply_to_message_id"].isString() &&
                parseId((*json)["reply_to_message_id"].asString(), replyTo) && replyTo != 0) {
                auto reply = api::make_object<api::inputMessageReplyToMessage>();
                reply->message_id_ = replyTo;
                fn->reply_to_ = std::move(reply);
            }
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

    // GET /v1/files/{fileId} — скачивание (§8.3′, решение C6): если файл докачан — стримим
    // содержимое (newFileResponse, sendfile, без буферизации в RAM); иначе инициируем докачку
    // и отдаём 202 + прогресс (готовность — по updateFile в WS, клиент опрашивает повторно).
    // fileId — эфемерный td file_id (валиден в рамках текущей сессии процесса).
    app.registerHandler(
        "/v1/files/{fileId}",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string fileIdStr) {
            std::int64_t fileId64 = 0;
            if (!parseId(fileIdStr, fileId64)) {
                cb(serviceError("VALIDATION_ERROR", "invalid file_id", drogon::k400BadRequest));
                return;
            }
            const auto fileId = static_cast<std::int32_t>(fileId64);
            [](tgw::bridge::TdBridge& td, std::int32_t cid, std::int32_t fid,
               HttpCallback callback) -> drogon::AsyncTask {
                auto fileObj = co_await td.invoke(cid, api::make_object<api::getFile>(fid));
                auto file = tgw::bridge::expect<api::file>(std::move(fileObj));
                if (!file.ok()) {
                    callback(telegramError(*file.error, drogon::k502BadGateway));
                    co_return;
                }
                const auto& local = file.value->local_;
                if (local != nullptr && local->is_downloading_completed_ && !local->path_.empty()) {
                    callback(drogon::HttpResponse::newFileResponse(local->path_));
                    co_return;
                }
                // Инициируем асинхронную докачку (synchronous=false); результат игнорируем.
                co_await td.invoke(cid, api::make_object<api::downloadFile>(fid, 1, 0, 0, false));
                Json::Value body;
                body["ok"] = true;
                body["data"] = tgw::dto::toJson(*file.value);
                callback(jsonResponse(std::move(body), drogon::k202Accepted));
                co_return;
            }(bridge, client_id, fileId, std::move(cb));
        },
        {drogon::Get, kBearerFilter});

    // POST /v1/chats/{chatId}/files — загрузка файла в чат (§8.3′, decision C10).
    // Сырое тело application/octet-stream пишется во временный файл, затем sendMessage как
    // документ. Тела > client_max_memory_body_size Drogon спулит на диск (mmap), поэтому
    // копирование ниже file->file через page cache, RSS не растёт с размером аплоада.
    // ?file_name=&caption= — опциональные. Temp-файл НЕ удаляется сразу (нужен TDLib на время
    // аплоада) — его снимает TTL-очистка (http/upload_cleanup, старт в main).
    app.registerHandler(
        "/v1/chats/{chatId}/files",
        [&bridge, client_id, upload_dir](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                                         std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            namespace fs = std::filesystem;
            std::string name = fs::path(req->getParameter("file_name")).filename().string();
            if (name.empty()) {
                name = "upload.bin";
            }
            static std::atomic<std::uint64_t> counter{0};
            const auto seq = counter.fetch_add(1, std::memory_order_relaxed) + 1;
            const fs::path dir = fs::path(upload_dir) / ("u" + std::to_string(seq));
            std::error_code ec;
            fs::create_directories(dir, ec);
            if (ec) {
                cb(serviceError("INTERNAL", "cannot create upload directory",
                                drogon::k500InternalServerError));
                return;
            }
            const fs::path path = dir / name;
            {
                std::ofstream out(path, std::ios::binary);
                out.write(req->bodyData(), static_cast<std::streamsize>(req->bodyLength()));
            }

            // ?type=document|photo|video|voice|audio — как отправить файл (default document).
            // У TDLib медиа-обёртки двухуровневые: inputMessageX.x_ : inputX, inputX.x_ :
            // InputFile.
            auto local_file = api::make_object<api::inputFileLocal>(path.string());
            api::object_ptr<api::formattedText> caption;
            {
                const std::string caption_text = req->getParameter("caption");
                if (!caption_text.empty()) {
                    caption = api::make_object<api::formattedText>();
                    caption->text_ = caption_text;
                }
            }
            api::object_ptr<api::InputMessageContent> content;
            const std::string media_type = req->getParameter("type");
            if (media_type.empty() || media_type == "document") {
                auto document = api::make_object<api::inputDocument>();
                document->document_ = std::move(local_file);
                auto msg = api::make_object<api::inputMessageDocument>();
                msg->document_ = std::move(document);
                msg->caption_ = std::move(caption);
                content = std::move(msg);
            } else if (media_type == "photo") {
                auto photo = api::make_object<api::inputPhoto>();
                photo->photo_ = std::move(local_file);
                auto msg = api::make_object<api::inputMessagePhoto>();
                msg->photo_ = std::move(photo);
                msg->caption_ = std::move(caption);
                content = std::move(msg);
            } else if (media_type == "video") {
                auto video = api::make_object<api::inputVideo>();
                video->video_ = std::move(local_file);
                video->supports_streaming_ = true;
                auto msg = api::make_object<api::inputMessageVideo>();
                msg->video_ = std::move(video);
                msg->caption_ = std::move(caption);
                content = std::move(msg);
            } else if (media_type == "voice") {
                auto msg = api::make_object<api::inputMessageVoiceNote>();
                msg->voice_note_ = std::move(local_file);
                msg->caption_ = std::move(caption);
                content = std::move(msg);
            } else if (media_type == "audio") {
                auto audio = api::make_object<api::inputAudio>();
                audio->audio_ = std::move(local_file);
                auto msg = api::make_object<api::inputMessageAudio>();
                msg->audio_ = std::move(audio);
                msg->caption_ = std::move(caption);
                content = std::move(msg);
            } else {
                cb(serviceError("VALIDATION_ERROR", "type must be document|photo|video|voice|audio",
                                drogon::k400BadRequest));
                return;
            }
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

    // PATCH /v1/chats/{chatId}/messages/{messageId} — правка текста. Тело: {"text", "parse_mode"?}.
    app.registerHandler(
        "/v1/chats/{chatId}/messages/{messageId}",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr, std::string messageIdStr) {
            std::int64_t chatId = 0;
            std::int64_t messageId = 0;
            if (!parseId(chatIdStr, chatId) || !parseId(messageIdStr, messageId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id/message_id",
                                drogon::k400BadRequest));
                return;
            }
            auto json = req->getJsonObject();
            if (json == nullptr || !(*json)["text"].isString() ||
                (*json)["text"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'text' is required",
                                drogon::k400BadRequest));
                return;
            }
            std::string parse_err;
            auto formatted = makeFormattedText(
                (*json)["text"].asString(),
                (*json)["parse_mode"].isString() ? (*json)["parse_mode"].asString() : "",
                parse_err);
            if (formatted == nullptr) {
                cb(serviceError("VALIDATION_ERROR", parse_err, drogon::k400BadRequest));
                return;
            }
            auto content = api::make_object<api::inputMessageText>();
            content->text_ = std::move(formatted);
            auto fn = api::make_object<api::editMessageText>();
            fn->chat_id_ = chatId;
            fn->message_id_ = messageId;
            fn->input_message_content_ = std::move(content);
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto message = tgw::bridge::expect<api::message>(std::move(obj));
                             if (!message.ok()) {
                                 return telegramError(*message.error, drogon::k502BadGateway);
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = tgw::dto::toJson(*message.value);
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Patch, kBearerFilter});

    // DELETE /v1/chats/{chatId}/messages — удаление. Тело: {"message_ids":[...], "revoke"?:true}.
    // revoke=true (default) — удалить у всех; false — только у себя.
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
            if (json == nullptr || !(*json)["message_ids"].isArray() ||
                (*json)["message_ids"].empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'message_ids' is required",
                                drogon::k400BadRequest));
                return;
            }
            auto fn = api::make_object<api::deleteMessages>();
            fn->chat_id_ = chatId;
            for (const auto& item : (*json)["message_ids"]) {
                std::int64_t id = 0;
                if (!item.isString() || !parseId(item.asString(), id)) {
                    cb(serviceError("VALIDATION_ERROR", "message_ids must be strings",
                                    drogon::k400BadRequest));
                    return;
                }
                fn->message_ids_.push_back(id);
            }
            fn->revoke_ = !(*json)["revoke"].isBool() || (*json)["revoke"].asBool();
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) -> drogon::HttpResponsePtr {
                             auto ok = tgw::bridge::expect<api::ok>(std::move(obj));
                             if (!ok.ok()) {
                                 return telegramError(*ok.error, drogon::k502BadGateway);
                             }
                             Json::Value body;
                             body["ok"] = true;
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Delete, kBearerFilter});

    // POST /v1/chats/{chatId}/messages/forward — пересылка. Тело: {"from_chat_id",
    // "message_ids":[...], "send_copy"?:false, "remove_caption"?:false}. 202 + временные id.
    app.registerHandler(
        "/v1/chats/{chatId}/messages/forward",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            auto json = req->getJsonObject();
            std::int64_t fromChatId = 0;
            if (json == nullptr || !(*json)["from_chat_id"].isString() ||
                !parseId((*json)["from_chat_id"].asString(), fromChatId) ||
                !(*json)["message_ids"].isArray() || (*json)["message_ids"].empty()) {
                cb(serviceError("VALIDATION_ERROR",
                                "fields 'from_chat_id' and 'message_ids' are required",
                                drogon::k400BadRequest));
                return;
            }
            auto fn = api::make_object<api::forwardMessages>();
            fn->chat_id_ = chatId;
            fn->from_chat_id_ = fromChatId;
            for (const auto& item : (*json)["message_ids"]) {
                std::int64_t id = 0;
                if (!item.isString() || !parseId(item.asString(), id)) {
                    cb(serviceError("VALIDATION_ERROR", "message_ids must be strings",
                                    drogon::k400BadRequest));
                    return;
                }
                fn->message_ids_.push_back(id);
            }
            fn->send_copy_ = (*json)["send_copy"].isBool() && (*json)["send_copy"].asBool();
            fn->remove_caption_ =
                (*json)["remove_caption"].isBool() && (*json)["remove_caption"].asBool();
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto messages = tgw::bridge::expect<api::messages>(std::move(obj));
                             if (!messages.ok()) {
                                 return telegramError(*messages.error, drogon::k502BadGateway);
                             }
                             Json::Value ids(Json::arrayValue);
                             for (const auto& m : messages.value->messages_) {
                                 if (m != nullptr) {
                                     ids.append(std::to_string(m->id_));
                                 }
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"]["temporary_message_ids"] = ids;
                             return jsonResponse(std::move(body), drogon::k202Accepted);
                         });
        },
        {drogon::Post, kBearerFilter});

    // Реакции. POST — поставить emoji-реакцию, DELETE — снять. Тело: {"emoji":"👍"}.
    // Фактическое изменение прилетит апдейтом updateMessageInteractionInfo по WebSocket.
    const auto okBuilder = [](api::object_ptr<api::Object> obj) -> drogon::HttpResponsePtr {
        if (obj != nullptr && obj->get_id() == api::error::ID) {
            return telegramError(static_cast<api::error&>(*obj), drogon::k502BadGateway);
        }
        Json::Value body;
        body["ok"] = true;
        return jsonResponse(std::move(body), drogon::k200OK);
    };

    // POST /v1/chats/{chatId}/messages/{messageId}/reactions
    app.registerHandler(
        "/v1/chats/{chatId}/messages/{messageId}/reactions",
        [&bridge, client_id, okBuilder](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                                        std::string chatIdStr, std::string messageIdStr) {
            std::int64_t chatId = 0;
            std::int64_t messageId = 0;
            if (!parseId(chatIdStr, chatId) || !parseId(messageIdStr, messageId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id/message_id",
                                drogon::k400BadRequest));
                return;
            }
            auto body = req->getJsonObject();
            if (body == nullptr || !(*body)["emoji"].isString() ||
                (*body)["emoji"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'emoji' is required",
                                drogon::k400BadRequest));
                return;
            }
            auto fn = api::make_object<api::addMessageReaction>();
            fn->chat_id_ = chatId;
            fn->message_id_ = messageId;
            fn->reaction_type_ =
                api::make_object<api::reactionTypeEmoji>((*body)["emoji"].asString());
            fn->is_big_ = false;
            fn->update_recent_reactions_ = true;
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb), okBuilder);
        },
        {drogon::Post, kBearerFilter});

    // DELETE /v1/chats/{chatId}/messages/{messageId}/reactions
    app.registerHandler(
        "/v1/chats/{chatId}/messages/{messageId}/reactions",
        [&bridge, client_id, okBuilder](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                                        std::string chatIdStr, std::string messageIdStr) {
            std::int64_t chatId = 0;
            std::int64_t messageId = 0;
            if (!parseId(chatIdStr, chatId) || !parseId(messageIdStr, messageId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id/message_id",
                                drogon::k400BadRequest));
                return;
            }
            auto body = req->getJsonObject();
            if (body == nullptr || !(*body)["emoji"].isString() ||
                (*body)["emoji"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'emoji' is required",
                                drogon::k400BadRequest));
                return;
            }
            auto fn = api::make_object<api::removeMessageReaction>();
            fn->chat_id_ = chatId;
            fn->message_id_ = messageId;
            fn->reaction_type_ =
                api::make_object<api::reactionTypeEmoji>((*body)["emoji"].asString());
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb), okBuilder);
        },
        {drogon::Delete, kBearerFilter});
}

}  // namespace tgw::http
