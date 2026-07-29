#include "bridge/expect.hpp"
#include "bridge/td_bridge.hpp"
#include "dto/file_dto.hpp"
#include "dto/message_dto.hpp"
#include "http/byte_range.hpp"
#include "http/http_helpers.hpp"
#include "http/idempotency_cache.hpp"
#include "http/routes.hpp"
#include "http/upload_sanitize.hpp"

#include <drogon/drogon.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <trantor/utils/ConcurrentTaskQueue.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace api = td::td_api;

namespace tgw::http {
namespace {

constexpr char kBearerFilter[] = "tgw::http::BearerAuthFilter";

// formattedText из text (+опц. parse_mode "markdown"|"html"). parseTextEntities — offline-метод
// TDLib: исполняется синхронно через ClientManager::execute, event-loop не задействован.
// При ошибке разметки/неизвестном режиме возвращает nullptr и заполняет err.
// (Разбор Range-заголовка вынесен в http/byte_range.hpp — чистая функция, покрыта тестом.)
// (jsonResponse/serviceError/telegramError/parseId/launchInvoke и IdempotencyCache вынесены в
//  http/http_helpers.hpp и http/idempotency_cache.hpp — решения 1.6 и 1.3.)

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

// Решение 1.5: пул под блокирующую запись аплоадов, ВНЕ IO-петель Drogon. Ленивая инициализация
// при первой загрузке; живёт до конца процесса. 2 треда — параллельная запись без монополизации.
trantor::ConcurrentTaskQueue& uploadTaskQueue() {
    static trantor::ConcurrentTaskQueue queue(2, "tgw-upload");
    return queue;
}

}  // namespace

// ВЕЗДЕ, где читаем тело: json->isObject() ПЕРЕД operator[](const char*)/isMember. jsoncpp
// разбирает `[]`, `"x"`, `5` как валидный документ, а Value::operator[](const char*) и
// Value::isMember на не-объекте кидают Json::LogicError. Обработчики registerHandler у Drogon
// (HttpControllerBinder) try/catch не оборачивают — исключение ушло бы наружу вместо честного 400.
void registerMessageRoutes(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                           const std::string& upload_dir) {
    auto& app = drogon::app();

    // GET /v1/chats — best-effort loadChats + getChats, затем getChat на каждый id (§8.2.4).
    // MVP: возможен частичный список сразу после старта (чаты подгружаются апдейтами) — норма.
    app.registerHandler(
        "/v1/chats",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            const std::int32_t limit = queryInt(req, "limit", 20, 100);
            // Решение 1.2: offset НЕЛЬЗЯ клампить к 900. Раньше он молча зажимался, а
            // meta.next_offset = off+lim рос до 1000 -> клиент по курсору вечно получал ту же
            // страницу (бесконечный цикл). Выход за верхнюю границу окна = явный 400 (не кламп).
            std::int32_t offset = 0;
            const std::string& offStr = req->getParameter("offset");
            if (!offStr.empty()) {
                try {
                    const int parsed = std::stoi(offStr);
                    if (parsed > 900) {
                        cb(serviceError("VALIDATION_ERROR", "offset must not exceed 900",
                                        drogon::k400BadRequest));
                        return;
                    }
                    if (parsed >= 1) {
                        offset = parsed;  // <1 -> 0 (как было в queryInt)
                    }
                } catch (...) {
                    // битое/переполнение -> 0 (как дефолт queryInt)
                }
            }
            [](tgw::bridge::TdBridge& td, std::int32_t cid, std::int32_t lim, std::int32_t off,
               HttpCallback callback) -> drogon::AsyncTask {
                // Пагинация: у getChats нет offset — берём off+lim id из главного списка и
                // отрезаем первые off (для main-list размеров это дёшево; лимит offset 900).
                const std::int32_t want = off + lim;
                co_await td.invoke(cid, makeLoadChats(want));  // прогрев, результат игнорируем
                auto chatsObj = co_await td.invoke(cid, makeGetChats(want));
                auto chats = tgw::bridge::expect<api::chats>(std::move(chatsObj));
                if (!chats.ok()) {
                    callback(telegramError(*chats.error));
                    co_return;
                }
                Json::Value arr(Json::arrayValue);
                const auto& ids = chats.value->chat_ids_;
                for (std::size_t i = static_cast<std::size_t>(off); i < ids.size(); ++i) {
                    auto chatObj = co_await td.invoke(cid, api::make_object<api::getChat>(ids[i]));
                    auto chat = tgw::bridge::expect<api::chat>(std::move(chatObj));
                    if (chat.ok()) {
                        arr.append(tgw::dto::toJson(*chat.value));
                    }
                }
                Json::Value body;
                body["ok"] = true;
                body["data"] = arr;
                // next_offset — если список, вероятно, не исчерпан. Решение 1.2: не эмитим
                // курсор за границей окна (want > 900) — по нему следующий запрос всё равно
                // получил бы 400, а клиент зациклился бы, повторяя один и тот же offset.
                if (ids.size() == static_cast<std::size_t>(want) && want <= 900) {
                    body["meta"]["next_offset"] = want;
                }
                callback(jsonResponse(std::move(body), drogon::k200OK));
                co_return;
            }(bridge, client_id, limit, offset, std::move(cb));
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
                                 return telegramError(*messages.error);
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
            if (json == nullptr || !json->isObject() || !(*json)["text"].isString() ||
                (*json)["text"].asString().empty()) {
                cb(serviceError("VALIDATION_ERROR", "field 'text' is required",
                                drogon::k400BadRequest));
                return;
            }
            // Idempotency-Key: повтор с тем же ключом отдаёт кэшированный ответ, не дублируя
            // отправку; ключ в полёте -> 409 IDEMPOTENCY_KEY_REUSED.
            const std::string idem_key = req->getHeader("Idempotency-Key");
            if (!idem_key.empty()) {
                int cached_status = 0;
                std::string cached_body;
                const int state =
                    IdempotencyCache::instance().claim(idem_key, cached_status, cached_body);
                if (state == 1) {
                    cb(serviceError("IDEMPOTENCY_KEY_REUSED",
                                    "request with this Idempotency-Key is in flight",
                                    drogon::k409Conflict));
                    return;
                }
                if (state == 2) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(cached_status));
                    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                    resp->addHeader("Idempotency-Replayed", "true");
                    resp->setBody(cached_body);
                    cb(resp);
                    return;
                }
            }
            std::string parse_err;
            auto formatted = makeFormattedText(
                (*json)["text"].asString(),
                (*json)["parse_mode"].isString() ? (*json)["parse_mode"].asString() : "",
                parse_err);
            if (formatted == nullptr) {
                if (!idem_key.empty()) {
                    IdempotencyCache::instance().release(idem_key);
                }
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
                         [idem_key](api::object_ptr<api::Object> obj) {
                             auto message = tgw::bridge::expect<api::message>(std::move(obj));
                             if (!message.ok()) {
                                 if (!idem_key.empty()) {
                                     // Ошибку не кэшируем — даём ретраю шанс.
                                     IdempotencyCache::instance().release(idem_key);
                                 }
                                 return telegramError(*message.error);
                             }
                             Json::Value data;
                             data["temporary_message_id"] = std::to_string(message.value->id_);
                             data["chat_id"] = std::to_string(message.value->chat_id_);
                             data["sending_state"] = "pending";
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = data;
                             auto resp = jsonResponse(std::move(body), drogon::k202Accepted);
                             if (!idem_key.empty()) {
                                 IdempotencyCache::instance().store(idem_key, resp->statusCode(),
                                                                    std::string(resp->body()));
                             }
                             return resp;
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
            if (json == nullptr || !json->isObject() || !(*json)["message_ids"].isArray()) {
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
                                 return telegramError(static_cast<api::error&>(*obj));
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
            // Решение 1.1: td file_id — int32. Без проверки static_cast<int32_t> усекал бы:
            // напр. 4294967297 -> 1, и getFile(1) вернул бы ЧУЖОЙ файл с 200. Значение вне
            // [1, INT32_MAX] — заведомо не наш file_id -> 400 (а не тихое усечение).
            if (fileId64 < 1 || fileId64 > std::numeric_limits<std::int32_t>::max()) {
                cb(serviceError("VALIDATION_ERROR", "invalid file_id", drogon::k400BadRequest));
                return;
            }
            const auto fileId = static_cast<std::int32_t>(fileId64);
            [](tgw::bridge::TdBridge& td, std::int32_t cid, std::int32_t fid,
               drogon::HttpRequestPtr request, HttpCallback callback) -> drogon::AsyncTask {
                auto fileObj = co_await td.invoke(cid, api::make_object<api::getFile>(fid));
                auto file = tgw::bridge::expect<api::file>(std::move(fileObj));
                if (!file.ok()) {
                    callback(telegramError(*file.error));
                    co_return;
                }
                const auto& local = file.value->local_;
                if (local != nullptr && local->is_downloading_completed_ && !local->path_.empty()) {
                    // Range/206: одиночный диапазон разбираем сами (decision C6).
                    const std::string range = request->getHeader("Range");
                    if (!range.empty()) {
                        std::error_code size_ec;
                        const std::uintmax_t fsize =
                            std::filesystem::file_size(local->path_, size_ec);
                        std::uintmax_t offset = 0;
                        std::uintmax_t length = 0;
                        if (!size_ec && parseByteRange(range, fsize, offset, length)) {
                            auto resp = drogon::HttpResponse::newFileResponse(
                                local->path_, static_cast<size_t>(offset),
                                static_cast<size_t>(length), true);
                            callback(resp);
                            co_return;
                        }
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
                        callback(resp);
                        co_return;
                    }
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
            }(bridge, client_id, fileId, req, std::move(cb));
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
            // Решение 1.5: запись сырого тела (потенциально многогигабайтного) и работа с ФС —
            // блокирующие. На IO-петле Drogon одна такая загрузка застопорила бы ВСЕ соединения
            // этой петли. Уносим всю обработку на отдельный пул, ответ отдаём из его колбэка.
            // Семантика ответов (в т.ч. 500 + очистка каталога при неудачной записи) неизменна —
            // меняется только поток исполнения. req/cb захватываем по значению: тело уже принято
            // целиком, callback можно вызвать из любого треда (Drogon сам перекинет в loop).
            uploadTaskQueue().runTaskInQueue([&bridge, client_id, upload_dir, chatId, req,
                                              cb = std::move(cb)]() mutable {
                namespace fs = std::filesystem;
                const std::string name = sanitizeUploadFilename(req->getParameter("file_name"));
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
                    // Диск полон / нет прав / путь не открылся: молчаливое продолжение отправило
                    // бы TDLib пустой/усечённый файл, и клиент получил бы вводящую в заблуждение
                    // TELEGRAM_ERROR вместо честной ошибки на нашей стороне. Чистим недописанный
                    // upload-каталог сразу — иначе он висел бы до TTL-очистки (upload_cleanup, 1ч).
                    if (!out) {
                        std::error_code cleanup_ec;
                        fs::remove_all(dir, cleanup_ec);
                        cb(serviceError("INTERNAL", "failed to write uploaded file",
                                        drogon::k500InternalServerError));
                        return;
                    }
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
                    cb(serviceError("VALIDATION_ERROR",
                                    "type must be document|photo|video|voice|audio",
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
                                     return telegramError(*message.error);
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
            if (json == nullptr || !json->isObject() || !(*json)["text"].isString() ||
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
                                 return telegramError(*message.error);
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
            if (json == nullptr || !json->isObject() || !(*json)["message_ids"].isArray() ||
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
                                 return telegramError(*ok.error);
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
            if (json == nullptr || !json->isObject() || !(*json)["from_chat_id"].isString() ||
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
                                 return telegramError(*messages.error);
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

    // GET /v1/chats/{chatId}/search?q=&limit=&from_id= — НАТИВНЫЙ поиск Telegram по чату
    // (searchChatMessages). В отличие от архиватора — прямой запрос к серверу Telegram.
    // from_id — курсор (meta.next_from_id прошлого вызова; 0 = с начала).
    app.registerHandler(
        "/v1/chats/{chatId}/search",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb,
                             std::string chatIdStr) {
            std::int64_t chatId = 0;
            if (!parseId(chatIdStr, chatId)) {
                cb(serviceError("VALIDATION_ERROR", "invalid chat_id", drogon::k400BadRequest));
                return;
            }
            const std::string query = req->getParameter("q");
            if (query.empty()) {
                cb(serviceError("VALIDATION_ERROR", "query param 'q' is required",
                                drogon::k400BadRequest));
                return;
            }
            std::int64_t fromId = 0;
            parseId(req->getParameter("from_id"), fromId);
            auto fn = api::make_object<api::searchChatMessages>();
            fn->chat_id_ = chatId;
            fn->query_ = query;
            fn->from_message_id_ = fromId;
            fn->offset_ = 0;
            fn->limit_ = queryInt(req, "limit", 20, 100);
            fn->filter_ = api::make_object<api::searchMessagesFilterEmpty>();
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto found =
                                 tgw::bridge::expect<api::foundChatMessages>(std::move(obj));
                             if (!found.ok()) {
                                 return telegramError(*found.error);
                             }
                             Json::Value arr(Json::arrayValue);
                             for (const auto& m : found.value->messages_) {
                                 if (m != nullptr) {
                                     arr.append(tgw::dto::toJson(*m));
                                 }
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = arr;
                             body["meta"]["total_count"] = found.value->total_count_;
                             if (found.value->next_from_message_id_ != 0) {
                                 body["meta"]["next_from_id"] =
                                     std::to_string(found.value->next_from_message_id_);
                             }
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Get, kBearerFilter});

    // GET /v1/search?q=&limit=&offset= — ГЛОБАЛЬНЫЙ нативный поиск Telegram по всем чатам
    // (searchMessages). offset — строковый курсор (meta.next_offset). Пусто = с начала.
    app.registerHandler(
        "/v1/search",
        [&bridge, client_id](const drogon::HttpRequestPtr& req, HttpCallback&& cb) {
            const std::string query = req->getParameter("q");
            if (query.empty()) {
                cb(serviceError("VALIDATION_ERROR", "query param 'q' is required",
                                drogon::k400BadRequest));
                return;
            }
            auto fn = api::make_object<api::searchMessages>();
            fn->query_ = query;
            fn->offset_ = req->getParameter("offset");
            fn->limit_ = queryInt(req, "limit", 20, 100);
            fn->filter_ = api::make_object<api::searchMessagesFilterEmpty>();
            fn->min_date_ = 0;
            fn->max_date_ = 0;
            launchInvoke(bridge, client_id, std::move(fn), std::move(cb),
                         [](api::object_ptr<api::Object> obj) {
                             auto found = tgw::bridge::expect<api::foundMessages>(std::move(obj));
                             if (!found.ok()) {
                                 return telegramError(*found.error);
                             }
                             Json::Value arr(Json::arrayValue);
                             for (const auto& m : found.value->messages_) {
                                 if (m != nullptr) {
                                     arr.append(tgw::dto::toJson(*m));
                                 }
                             }
                             Json::Value body;
                             body["ok"] = true;
                             body["data"] = arr;
                             body["meta"]["total_count"] = found.value->total_count_;
                             if (!found.value->next_offset_.empty()) {
                                 body["meta"]["next_offset"] = found.value->next_offset_;
                             }
                             return jsonResponse(std::move(body), drogon::k200OK);
                         });
        },
        {drogon::Get, kBearerFilter});

    // Реакции. POST — поставить emoji-реакцию, DELETE — снять. Тело: {"emoji":"👍"}.
    // Фактическое изменение прилетит апдейтом updateMessageInteractionInfo по WebSocket.
    const auto okBuilder = [](api::object_ptr<api::Object> obj) -> drogon::HttpResponsePtr {
        if (obj != nullptr && obj->get_id() == api::error::ID) {
            return telegramError(static_cast<api::error&>(*obj));
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
            if (body == nullptr || !body->isObject() || !(*body)["emoji"].isString() ||
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
            if (body == nullptr || !body->isObject() || !(*body)["emoji"].isString() ||
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
