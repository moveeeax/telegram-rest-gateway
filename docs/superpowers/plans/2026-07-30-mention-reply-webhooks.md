# Mention/Reply Owner Webhooks — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Доставлять полный контекст входящего сообщения (и reply-цепочку) на зарегистрированные HTTP-вебхуки, когда затронут владелец аккаунта (mention / reply-на-его-сообщение / любое DM).

**Architecture:** На `updateNewMessage` в `UpdateRouter` чистый детектор решает, триггер ли это. При триггере async-задача на `TdBridge` строит расширенную JSON-проекцию и поднимает reply-цепочку через `getMessage`, затем кладёт событие в `WebhookDispatcher` — выделенный воркер-пул, который POST-ит его всем `active` вебхукам из `WebhookRegistry` (in-memory + персист в S3 per session), подписывая HMAC-SHA256. Всё вне потока-приёмника TDLib и IO-петель Drogon.

**Tech Stack:** C++20, Drogon v1.9.8, TDLib, OpenSSL (HMAC/SHA через `tgw::util`), jsoncpp, gtest. S3 через существующий `tgw::util::S3Client`. Исходящий HTTP через `drogon::HttpClient` (образец — `src/util/s3_client.cpp`).

## Global Constraints

- Рабочая директория: git worktree `/Users/moveeeax/Public/github/telegram-rest-gateway-webhooks`, ветка `feat/mention-reply-webhooks`. Только там.
- Комментарии в коде — по-русски, стиль репо (объясняют «зачем»/инвариант). Эталоны извлечений: `src/http/byte_range.hpp`, `src/http/scope_policy.hpp`, `src/ws/update_router.*`.
- Сборка с `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` — без сужений и теней.
- Коммиты: conventional commits (`feat(...)`, `test(...)`, `docs(...)`). СТРОГО ЗАПРЕЩЕНЫ трейлеры атрибуции ИИ (`Co-Authored-By: Claude`, `Generated with Claude Code` и любые). Обычные сообщения.
- Локальную сборку имплементер НЕ запускает (нет тулчейна) — верифицирует контроллер (нативный arm64 builder `docker.io/resert/telegram-rest-gateway:builder-arm64`, `--security-opt seccomp=unconfined` для TSan) и CI после пуша. Обязателен статический self-review каждого файла + сверка имён td_api-типов по `<td/telegram/td_api.h>`.
- Новые кросс-поточные компоненты (`WebhookRegistry`, `WebhookDispatcher`, интеграция в `UpdateRouter`) обязаны быть TSan-чистыми.
- Новые .cpp ядра (детектор header-only, context_builder, webhook_registry, webhook_dispatcher) добавляются в статическую либу `tgw_bridge` (CMakeLists.txt:61-77) — чтобы юнит-тесты линковались, как `ws_registry`. REST-роуты (`webhook_routes.cpp`) — в executable `telegram-rest-gateway` (CMakeLists.txt:84+).
- Все новые тест-файлы регистрируются в `tests/CMakeLists.txt` по образцу существующих; стиль тестов — `tests/byte_range_test.cpp`, `tests/update_router_test.cpp`, `tests/session_io_test.cpp`.
- Фича целиком под мастер-флагом `TGW_WEBHOOKS_ENABLED` (default false): выключено — ноль накладных расходов в горячем пути `onUpdate`.

Спека: `docs/superpowers/specs/2026-07-30-mention-reply-webhooks-design.md`.

---

### Task 1: Конфиг фичи

**Files:**
- Modify: `src/config/config.hpp` (добавить поля), `src/config/config.cpp` (парсинг)
- Test: `tests/config_test.cpp` (добавить кейсы)

**Interfaces:**
- Produces: поля `Config`: `bool webhooks_enabled = false;`, `int webhook_timeout_ms = 10000;`, `std::size_t webhook_queue_max = 10000;`, `bool webhook_ssrf_guard = false;`

- [ ] **Step 1: Тест парсинга (добавить в config_test.cpp)**
```cpp
TEST(ConfigTest, WebhookFlagsParsing) {
    EnvGuard g;  // существующий helper сохранения/восстановления окружения (см. файл)
    setenv("TGW_WEBHOOKS_ENABLED", "true", 1);
    setenv("TGW_WEBHOOK_TIMEOUT_MS", "5000", 1);
    setenv("TGW_WEBHOOK_QUEUE_MAX", "500", 1);
    setenv("TGW_WEBHOOK_SSRF_GUARD", "1", 1);
    const auto c = tgw::config::loadConfig();
    EXPECT_TRUE(c.webhooks_enabled);
    EXPECT_EQ(c.webhook_timeout_ms, 5000);
    EXPECT_EQ(c.webhook_queue_max, 500u);
    EXPECT_TRUE(c.webhook_ssrf_guard);
}
TEST(ConfigTest, WebhookFlagsDefaults) {
    EnvGuard g;
    unsetenv("TGW_WEBHOOKS_ENABLED");
    const auto c = tgw::config::loadConfig();
    EXPECT_FALSE(c.webhooks_enabled);
    EXPECT_EQ(c.webhook_timeout_ms, 10000);
}
```
(Сверь точное имя env-хелпера в config_test.cpp — используй существующий паттерн сохранения/восстановления, как в `KeepOnlineFlagParsing`. Добавь `TGW_WEBHOOK*` в список env, который тест чистит.)

- [ ] **Step 2: Прогон — FAIL** (полей нет). Контроллер: локальная сборка тестов.

- [ ] **Step 3: Поля в config.hpp** (рядом с `keep_online`):
```cpp
bool webhooks_enabled = false;
int webhook_timeout_ms = 10000;
std::size_t webhook_queue_max = 10000;
bool webhook_ssrf_guard = false;
```

- [ ] **Step 4: Парсинг в config.cpp** (по образцу `keep_online` / числовых через существующий `parseNumericEnv`):
```cpp
const std::string wh = envOrFile("TGW_WEBHOOKS_ENABLED");
c.webhooks_enabled = (wh == "1" || wh == "true");
c.webhook_timeout_ms = parseNumericEnv<int>("TGW_WEBHOOK_TIMEOUT_MS", 10000);
c.webhook_queue_max = parseNumericEnv<std::size_t>("TGW_WEBHOOK_QUEUE_MAX", 10000);
const std::string wg = envOrFile("TGW_WEBHOOK_SSRF_GUARD");
c.webhook_ssrf_guard = (wg == "1" || wg == "true");
```
(Сверь сигнатуру `parseNumericEnv` — она добавлена в config.cpp предыдущей фичей; используй её реальную форму.)

- [ ] **Step 5: Прогон — PASS.** Commit: `git add src/config tests/config_test.cpp && git commit -m "feat(config): webhook feature flags"`

---

### Task 2: OwnerMentionDetector (чистая детекция триггера)

**Files:**
- Create: `src/ws/owner_mention_detector.hpp` (header-only, чистые функции)
- Test: `tests/owner_mention_detector_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
```cpp
namespace tgw::ws {
enum class TriggerReason { Dm, Mention, Reply };
// reply_pending: mention/dm не сработали, но есть reply_to — нужен async-резолв автора родителя.
struct DetectResult {
    bool triggered = false;      // точно триггер (dm/mention) без резолва
    bool reply_pending = false;  // требуется async getMessage(родитель)
    TriggerReason reason = TriggerReason::Dm;
};
// owner_id: id владельца (0 = ещё не известен -> mention/reply по owner недоступны, но dm работает).
DetectResult detect(const td::td_api::message& msg, std::int64_t owner_id, bool chat_is_private,
                    bool chat_is_broadcast);
}
```

- [ ] **Step 1: Тесты.** Строим `td_api::message` через `make_object`, выставляем поля. Проверь точные имена: `message.is_outgoing_`, `message.contains_unread_mention_`, `message.sender_id_` (messageSenderUser::user_id_), `message.reply_to_` (тип `MessageReplyTo`; для reply-на-сообщение — `messageReplyToMessage`).
```cpp
namespace api = td::td_api;
using tgw::ws::detect; using tgw::ws::TriggerReason;

static api::object_ptr<api::message> mkMsg(bool outgoing, std::int64_t sender_uid,
        bool mention, bool has_reply) {
    auto m = api::make_object<api::message>();
    m->is_outgoing_ = outgoing;
    m->contains_unread_mention_ = mention;
    m->sender_id_ = api::make_object<api::messageSenderUser>(sender_uid);
    if (has_reply) m->reply_to_ = api::make_object<api::messageReplyToMessage>();
    return m;
}

TEST(OwnerMentionDetector, DmAnyIncomingTriggers) {
    auto m = mkMsg(/*outgoing*/false, /*sender*/555, false, false);
    auto r = detect(*m, /*owner*/111, /*private*/true, /*broadcast*/false);
    EXPECT_TRUE(r.triggered); EXPECT_EQ(r.reason, TriggerReason::Dm);
}
TEST(OwnerMentionDetector, OutgoingNeverTriggers) {
    auto m = mkMsg(true, 111, true, true);
    auto r = detect(*m, 111, true, false);
    EXPECT_FALSE(r.triggered); EXPECT_FALSE(r.reply_pending);
}
TEST(OwnerMentionDetector, MentionFlagTriggersInGroup) {
    auto m = mkMsg(false, 555, /*mention*/true, false);
    auto r = detect(*m, 111, /*private*/false, false);
    EXPECT_TRUE(r.triggered); EXPECT_EQ(r.reason, TriggerReason::Mention);
}
TEST(OwnerMentionDetector, ReplyInGroupIsPending) {
    auto m = mkMsg(false, 555, /*mention*/false, /*reply*/true);
    auto r = detect(*m, 111, /*private*/false, false);
    EXPECT_FALSE(r.triggered); EXPECT_TRUE(r.reply_pending);
    EXPECT_EQ(r.reason, TriggerReason::Reply);
}
TEST(OwnerMentionDetector, BroadcastChannelNeverTriggers) {
    auto m = mkMsg(false, 555, true, true);
    auto r = detect(*m, 111, false, /*broadcast*/true);
    EXPECT_FALSE(r.triggered); EXPECT_FALSE(r.reply_pending);
}
TEST(OwnerMentionDetector, OwnSenderNotTrigger) {
    auto m = mkMsg(false, /*sender==owner*/111, false, false);
    auto r = detect(*m, 111, true, false);
    EXPECT_FALSE(r.triggered);
}
TEST(OwnerMentionDetector, BotSenderStillTriggersDm) {  // ботов не фильтруем
    auto m = mkMsg(false, 777, false, false);
    auto r = detect(*m, 111, true, false);
    EXPECT_TRUE(r.triggered);
}
```

- [ ] **Step 2: Прогон — FAIL** (нет заголовка).

- [ ] **Step 3: Реализация `owner_mention_detector.hpp`.** Логика: broadcast или outgoing или sender==owner → `{}`. Иначе: private → Dm triggered. mention-флаг → Mention triggered. reply_to==messageReplyToMessage → reply_pending, reason=Reply. Приоритет: broadcast/outgoing/own отсекают; затем mention > (private Dm) — но dm тоже сразу triggered; если и mention и private — reason Mention (приоритет из спеки mention>reply>dm). Пиши явно:
```cpp
#pragma once
#include <td/telegram/td_api.h>
#include <cstdint>
namespace tgw::ws {
enum class TriggerReason { Dm, Mention, Reply };
struct DetectResult { bool triggered=false; bool reply_pending=false; TriggerReason reason=TriggerReason::Dm; };

inline DetectResult detect(const td::td_api::message& msg, std::int64_t owner_id,
                           bool chat_is_private, bool chat_is_broadcast) {
    namespace api = td::td_api;
    if (chat_is_broadcast || msg.is_outgoing_) return {};
    // отправитель == владелец -> не наш случай
    if (msg.sender_id_ != nullptr && msg.sender_id_->get_id() == api::messageSenderUser::ID &&
        static_cast<const api::messageSenderUser&>(*msg.sender_id_).user_id_ == owner_id) return {};
    if (msg.contains_unread_mention_) return {true, false, TriggerReason::Mention};
    if (chat_is_private) return {true, false, TriggerReason::Dm};
    const bool is_reply = msg.reply_to_ != nullptr &&
        msg.reply_to_->get_id() == api::messageReplyToMessage::ID;
    if (is_reply) return {false, true, TriggerReason::Reply};
    return {};
}
}
```
(owner_id==0 → sender==owner ложно, mention/dm работают, reply_pending ставится — но async-резолв при owner_id==0 отменит. Приемлемо: до готовности owner_id reply просто не подтвердится.)

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Регистрация теста в tests/CMakeLists.txt** (добавь `owner_mention_detector_test.cpp` в список исходников юнит-таргета по образцу соседних). Commit: `git add src/ws/owner_mention_detector.hpp tests/owner_mention_detector_test.cpp tests/CMakeLists.txt && git commit -m "feat(ws): owner mention/reply trigger detector"`

---

### Task 3: Расширенная JSON-проекция сообщения

**Files:**
- Modify: `src/dto/message_dto.hpp` (объявить), `src/dto/message_dto.cpp` (реализовать)
- Test: `tests/message_dto_test.cpp`

**Interfaces:**
- Produces: `Json::Value tgw::dto::webhookMessageToJson(const td::td_api::message& msg);` — проекция по спеке (id, chat{id}, sender{id,is_bot}, date, text, entities[], reply_to_message_id, attachment{...}). Существующая `toJson(const message&)` НЕ меняется.

- [ ] **Step 1: Тест** — сообщение с текстом+entity+reply, проверить поля проекции.
```cpp
TEST(MessageDto, WebhookProjectionTextReplyEntities) {
    namespace api = td::td_api;
    auto m = api::make_object<api::message>();
    m->id_ = 4200; m->chat_id_ = -100500; m->date_ = 1730000000;
    m->is_outgoing_ = false;
    m->sender_id_ = api::make_object<api::messageSenderUser>(555);
    auto ft = api::make_object<api::formattedText>();
    ft->text_ = "hi @me";
    auto ent = api::make_object<api::textEntity>();
    ent->offset_ = 3; ent->length_ = 3;
    ent->type_ = api::make_object<api::textEntityTypeMention>();
    ft->entities_.push_back(std::move(ent));
    m->content_ = api::make_object<api::messageText>(std::move(ft), nullptr, nullptr);
    m->reply_to_ = api::make_object<api::messageReplyToMessage>();
    static_cast<api::messageReplyToMessage&>(*m->reply_to_).message_id_ = 4100;

    const Json::Value j = tgw::dto::webhookMessageToJson(*m);
    EXPECT_EQ(j["id"].asString(), "4200");
    EXPECT_EQ(j["chat"]["id"].asString(), "-100500");
    EXPECT_EQ(j["sender"]["id"].asString(), "555");
    EXPECT_EQ(j["text"].asString(), "hi @me");
    ASSERT_TRUE(j["entities"].isArray());
    EXPECT_EQ(j["entities"][0]["type"].asString(), "mention");
    EXPECT_EQ(j["reply_to_message_id"].asString(), "4100");
}
```
(Сверь конструктор `messageText` и поля `messageReplyToMessage` — `message_id_`, возможно `chat_id_`, `origin_`; используй фактические из td_api.h. Для attachment переиспользуй существующий `contentToJson` как основу типа/файла — не дублируй логику вложений, вынеси общее.)

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация `webhookMessageToJson`** в message_dto.cpp. Собери проекцию: базовые поля; `sender` из `sender_id_` (messageSenderUser→user_id, is_bot — если доступно, иначе опустить/false); `text` и `entities` из content (messageText.text_ или caption); `attachment` — переиспользуя тип/file_id из существующего `contentToJson`; `reply_to_message_id` из messageReplyToMessage. entity-тип маппится в короткую строку (`mention`, `mention_name`, `hashtag`, `url`, `bold`, ... — минимум `mention`/`mention_name` обязательны, остальные best-effort). НЕ логируй тела.

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Commit:** `git add src/dto/message_dto.* tests/message_dto_test.cpp && git commit -m "feat(dto): extended webhook message projection"`

---

### Task 4: WebhookRegistry (in-memory + S3)

**Files:**
- Create: `src/webhook/webhook_registry.hpp`, `src/webhook/webhook_registry.cpp`
- Test: `tests/webhook_registry_test.cpp`
- Modify: `CMakeLists.txt` (добавить .cpp в tgw_bridge), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `tgw::util::S3Client` (`get()`, `put(body)`, `Result{http_status, body, error, ok(), notFound()}`).
- Produces:
```cpp
namespace tgw::webhook {
struct Webhook { std::string id; std::string url; std::string secret; bool active=true; };
// Абстракция хранилища ради тестируемости без сети.
class IWebhookStore { public: virtual ~IWebhookStore()=default;
    virtual std::optional<std::string> load()=0;          // сырой JSON или nullopt
    virtual bool save(const std::string& json)=0; };
class WebhookRegistry {
  public:
    explicit WebhookRegistry(IWebhookStore& store);
    void loadFromStore();                                 // при старте; отсутствие/ошибка -> пустой + лог
    std::string add(const std::string& url, const std::string& secret, bool active); // -> id (uuid)
    bool remove(const std::string& id);
    std::vector<Webhook> list() const;                    // копия под mutex
    std::vector<Webhook> activeSnapshot() const;          // только active, копия под mutex
  private:
    mutable std::mutex m_; std::vector<Webhook> hooks_; IWebhookStore& store_;
    void persist();                                       // сериализовать + store_.save
};
// Реализация IWebhookStore поверх S3Client (в .cpp).
}
```
uuid: без внешних либ — сгенерировать из счётчика+времени? Нет (Date/random политика). Используй `tgw::util` при наличии генератора; иначе детерминированный id = hex(sha256(url)[0:16]) через `tgw::util::sha256Hex` (стабилен, коллизии по разным url маловероятны; дубль url → тот же id, приемлемо).

- [ ] **Step 1: Тесты на фейковом сторе.**
```cpp
struct FakeStore : tgw::webhook::IWebhookStore {
    std::optional<std::string> data; bool fail_save=false;
    std::optional<std::string> load() override { return data; }
    bool save(const std::string& j) override { if(fail_save) return false; data=j; return true; }
};
TEST(WebhookRegistry, AddListPersist) {
    FakeStore s; tgw::webhook::WebhookRegistry r(s);
    auto id = r.add("https://h/1", "sekret", true);
    EXPECT_FALSE(id.empty());
    ASSERT_TRUE(s.data.has_value());              // персистнули
    auto l = r.list(); ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l[0].url, "https://h/1");
}
TEST(WebhookRegistry, LoadFromStoreRoundTrip) {
    FakeStore s; { tgw::webhook::WebhookRegistry r(s); r.add("https://h/1","x",true); }
    tgw::webhook::WebhookRegistry r2(s); r2.loadFromStore();
    ASSERT_EQ(r2.list().size(), 1u);
    EXPECT_EQ(r2.activeSnapshot().size(), 1u);
}
TEST(WebhookRegistry, RemoveAndInactive) {
    FakeStore s; tgw::webhook::WebhookRegistry r(s);
    auto id=r.add("https://h/1","x",false);
    EXPECT_EQ(r.activeSnapshot().size(), 0u);     // inactive не в snapshot
    EXPECT_TRUE(r.remove(id));
    EXPECT_EQ(r.list().size(), 0u);
}
TEST(WebhookRegistry, EmptyStoreLoadsEmpty) {
    FakeStore s; tgw::webhook::WebhookRegistry r(s); r.loadFromStore();
    EXPECT_EQ(r.list().size(), 0u);
}
```

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.** JSON (jsoncpp) массив объектов {id,url,secret,active}. `add`: сгенерить id, push, persist, вернуть id. `remove`: стереть по id, persist. `activeSnapshot`: копия active под mutex. `loadFromStore`: `store_.load()` → parse; ошибка парсинга/nullopt → пустой + LOG_WARN. `persist`: сериализовать → `store_.save`; false → LOG_ERROR + метрика. S3-реализация `IWebhookStore`: обёртка над `S3Client` (get→load: notFound→nullopt, ok→body; put→save).

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: CMake + commit.** Добавь `src/webhook/webhook_registry.cpp` в tgw_bridge; тест в tests/CMakeLists.txt. `git commit -m "feat(webhook): registry with S3-backed persistence"`

---

### Task 5: ContextBuilder (async проекция + reply-цепочка)

**Files:**
- Create: `src/webhook/context_builder.hpp`, `src/webhook/context_builder.cpp`
- Test: `tests/context_builder_test.cpp` (на `FakeTdTransport`)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `tgw::bridge::TdBridge::invoke(client_id, fn) -> TdAwaitable` (co_await → object_ptr<Object>); `tgw::dto::webhookMessageToJson`; `tgw::ws::DetectResult`.
- Produces:
```cpp
namespace tgw::webhook {
struct WebhookEvent { std::string event_id, session_id, owner_id, trigger_reason;
                      std::int32_t received_at=0; Json::Value message; Json::Value reply_chain;
                      bool chain_truncated=false; };
// Async: строит событие. Возвращает nullopt, если reply_pending и автор родителя != owner
// (т.е. триггер не подтвердился). client_id/owner_id/session_id — из вызывающего.
drogon::Task<std::optional<WebhookEvent>> buildEvent(
    tgw::bridge::TdBridge& bridge, std::int32_t client_id,
    const td::td_api::message& msg, tgw::ws::DetectResult det,
    std::int64_t owner_id, std::string session_id, std::int32_t received_at,
    int chain_limit /*=20*/);
}
```
`received_at` передаётся снаружи (в тестах — фикс; в проде — из `msg.date_` или часов моста, НЕ `Date::now()` в чистой логике).

- [ ] **Step 1: Тест на FakeTdTransport** (образец `tests/td_bridge_test.cpp` — тот же `BridgeHarness`/`LoopThread`, скриптованные ответы getMessage).
```cpp
// getMessage(chat,msg) отвечает message с заданным sender/reply_to — строим цепочку.
TEST(ContextBuilder, ReplyChainToRootWithLimit) {
    // Скрипт: msg 4100 -> reply 4090 -> reply 4080 -> (нет reply, корень)
    // owner=111. Родитель 4100 отправлен owner -> reply подтверждён.
    // Проверить: reply_chain размер 3, порядок родитель->корень, chain_truncated=false.
}
TEST(ContextBuilder, ReplyNotFromOwnerReturnsNullopt) {
    // det.reply_pending=true, родитель отправлен НЕ owner -> nullopt.
}
TEST(ContextBuilder, ChainTruncatedAtLimit) {
    // цепочка длиннее лимита (limit=2) -> chain_truncated=true, ровно 2 звена.
}
TEST(ContextBuilder, GetMessageFailureTruncates) {
    // getMessage вернул error на 2-м звене -> частичная цепочка + truncated=true.
}
```
(Разверни тела: скриптуй `FakeTdTransport` отвечать на `getMessage` нужными message-объектами по (chat_id,message_id); гоняй корутину на реальном loop через harness, снимай результат через promise/future — как в td_bridge_test. Сверь `getMessage` API: `td_api::getMessage(chat_id_, message_id_)` и поле `messageReplyToMessage.message_id_` для подъёма.)

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация `buildEvent` (coroutine).** Псевдо-структура:
```cpp
drogon::Task<std::optional<WebhookEvent>> buildEvent(...) {
    WebhookEvent ev;
    ev.session_id = session_id; ev.owner_id = std::to_string(owner_id);
    ev.received_at = received_at;
    ev.message = tgw::dto::webhookMessageToJson(msg);
    ev.event_id = session_id + ":" + std::to_string(msg.chat_id_) + ":" + std::to_string(msg.id_);
    ev.reply_chain = Json::Value(Json::arrayValue);
    // родитель, если есть reply_to
    std::optional<std::int64_t> parent_id = replyParentId(msg); // messageReplyToMessage.message_id_
    bool truncated = false;
    if (parent_id) {
        std::int64_t cur = *parent_id; int hops = 0;
        while (cur != 0 && hops < chain_limit) {
            auto obj = co_await bridge.invoke(client_id,
                td::td_api::make_object<td::td_api::getMessage>(msg.chat_id_, cur));
            if (!obj || obj->get_id() == td::td_api::error::ID) { truncated = true; break; }
            const auto& parent = static_cast<const td::td_api::message&>(*obj);
            if (hops == 0 && det.reply_pending) {          // проверка reply-владельца на 1-м звене
                if (!senderIsOwner(parent, owner_id)) co_return std::nullopt;
                ev.trigger_reason = "reply";
            }
            ev.reply_chain.append(tgw::dto::webhookMessageToJson(parent));
            cur = replyParentId(parent).value_or(0);
            ++hops;
        }
        if (cur != 0 && parent_id) truncated = truncated || (hops >= chain_limit);
    } else if (det.reply_pending) {
        co_return std::nullopt; // reply_pending без родителя невозможно -> не триггер
    }
    ev.chain_truncated = truncated;
    if (ev.trigger_reason.empty())
        ev.trigger_reason = (det.reason==TriggerReason::Mention?"mention":
                             det.reason==TriggerReason::Dm?"dm":"reply");
    co_return ev;
}
```
Хелперы `replyParentId`, `senderIsOwner` — статические в .cpp. Сверь тип корутины (`drogon::Task<T>`) и что `invoke` co_await-абелен из неё.

- [ ] **Step 4: Прогон — PASS (dev-debug + tsan).**

- [ ] **Step 5: CMake + commit** `feat(webhook): async context builder with reply chain`

---

### Task 6: WebhookDispatcher (воркер-пул, HMAC, доставка)

**Files:**
- Create: `src/webhook/webhook_dispatcher.hpp`, `src/webhook/webhook_dispatcher.cpp`
- Test: `tests/webhook_dispatcher_test.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/util/metrics.hpp` (счётчики)

**Interfaces:**
- Consumes: `WebhookRegistry::activeSnapshot()`, `WebhookEvent`, `tgw::util::hmacSha256`, `tgw::util::sha256Hex`.
- Produces:
```cpp
namespace tgw::webhook {
// Подпись — чистая функция, тестируется отдельно от сети.
std::string signBody(const std::string& secret, const std::string& body); // "sha256=<hex>"
class WebhookDispatcher {
  public:
    WebhookDispatcher(WebhookRegistry& reg, int timeout_ms, std::size_t queue_max, bool ssrf_guard);
    ~WebhookDispatcher();                       // джойн воркера
    void start();                               // поднять воркер-loop
    void dispatch(const WebhookEvent& ev);      // неблокирующе: сериализовать + enqueue (drop при переполнении)
    void stop();
  private:
    // очередь payload'ов + trantor::EventLoopThread + HttpClient-паттерн (образец s3_client.cpp:44-72)
};
}
```
Метрики (в metrics.hpp, по образцу существующих atomic-счётчиков): `webhook_delivered_total`, `webhook_failed_total`, `webhook_dropped_total`.

- [ ] **Step 1: Тест подписи (известный вектор) + доставки на мок-HTTP.**
```cpp
TEST(WebhookDispatcher, HmacSignatureKnownVector) {
    // Эталон: printf '%s' 'hi' | openssl dgst -sha256 -hmac 'key'
    auto sig = tgw::webhook::signBody("key", "hi");
    EXPECT_EQ(sig,
        "sha256=1c9dc82e5f8e5ed5a0180aad33b8204dea12fde2fb62ffb5e963035bf324a7a4");
}
```
Доставка: подними мок-HTTP на 127.0.0.1:0 (drogon app в тесте недоступен просто — используй лёгкий приёмный сокет или trantor TcpServer; принимай POST, сохраняй заголовки/тело, отвечай 200). Проверь: получен `X-TGW-Signature`, `X-TGW-Event-Id`, тело == сериализованное событие, инкремент `webhook_delivered_total`. Второй кейс: приёмник отвечает 500 → `webhook_failed_total`. Третий: невалидный/недоступный порт → failed. (Если поднять приёмник в юнит-процессе сложно/флаки под TSan — минимально покрой `signBody` вектором и сериализацию payload, а сетевую доставку вынеси в отдельный best-effort кейс с коротким таймаутом; задокументируй в отчёте.)

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.** `signBody`: `hmacSha256(secret, body)` (сырые байты) → hex → `"sha256="+hex`. Добавь hex-хелпер (или используй существующий — `hmacSha256` даёт байты, захексь вручную). Воркер: `trantor::EventLoopThread`; `dispatch` сериализует `WebhookEvent` в JSON-строку (компактно, как `compact()` в update_router), кладёт в очередь под mutex+cv (drop при size>=queue_max → `webhook_dropped_total`); воркер достаёт, для каждого active-вебхука `drogon::HttpClient` POST с заголовками, таймаут `timeout_ms`, по результату инкремент delivered/failed. SSRF-guard (если включён): отклонять url с приватным/loopback хостом до отправки. Всё — только на воркер-loop (librdkafka-инвариант-аналог: HttpClient трогается из одного loop). Джойн в stop()/dtor.

- [ ] **Step 4: Прогон — PASS (tsan обязателен).**

- [ ] **Step 5: CMake + metrics + commit** `feat(webhook): dispatcher with HMAC-signed fire-and-forget delivery`

---

### Task 7: REST-роуты /v1/webhooks

**Files:**
- Create: `src/http/webhook_routes.hpp`, `src/http/webhook_routes.cpp`
- Modify: `src/http/route_table.hpp` (добавить пути в единую таблицу — источник истины регистрации), `src/http/routes.cpp` или регистрация где заводится registry, `CMakeLists.txt` (executable), `tests/route_smoke_test.cpp`

**Interfaces:**
- Consumes: `WebhookRegistry`, общие хелперы `src/http/http_helpers.hpp` (`jsonResponse`, `serviceError`, `parseId`), `kBearerFilter`, `route_table.hpp`.
- Produces: `void registerWebhookRoutes(WebhookRegistry& reg);`

- [ ] **Step 1: route-smoke кейсы.** По образцу существующего `route_smoke_test.cpp` (таблица маршрутов ↔ фильтр): добавить `/v1/webhooks` (GET/POST) и `/v1/webhooks/{id}` (DELETE) в `kRoutesTable` как protected (admin) и проверить, что без токена → 401. Плюс юнит на парсинг тела POST (валидный `{url,secret}` → 200+id; отсутствие url → 400 VALIDATION_ERROR).

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.** `POST /v1/webhooks`: type-check JSON тела (как в message_routes — проверка типов до operator[]), `url` обязателен (иначе 400), `secret` опционален, `active` default true → `reg.add(...)` → 200 `{id,url,active}`. `GET /v1/webhooks`: `reg.list()` без `secret` → массив. `DELETE /v1/webhooks/{id}`: `reg.remove(id)` → 200 или 404. Добавить пути в `route_table.hpp` (единый источник — иначе route-smoke поймает рассинхрон) с `requires_auth=true`; scope admin проверяется bearer-фильтром/scope_policy (сверь, как scope навешивается — если нужен admin-scope маркер в таблице, добавь его согласованно с существующим механизмом `requiredScopeFor`).

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Commit** `feat(http): /v1/webhooks registration endpoints`

---

### Task 8: Интеграция в UpdateRouter + owner_id + wiring в main

**Files:**
- Modify: `src/ws/update_router.hpp`/`.cpp` (хук на updateNewMessage), `src/main.cpp` (wiring registry/dispatcher/owner_id, gating флагом), `src/auth/auth_state_manager.hpp` (при необходимости — но owner_id резолвим в setOnReady), `tests/update_router_test.cpp`
- Test: `tests/update_router_test.cpp`

**Interfaces:**
- Consumes: `detect()`, `buildEvent()`, `WebhookDispatcher`, `WebhookRegistry`, `TdBridge`.
- Produces: колбэк-хук `UpdateRouter::setWebhookHook(std::function<void(const td::td_api::message&)> )` (по образцу `setEventPublisher`/`setOnConnectionReady`) — вызывается на updateNewMessage; сам хук в main запускает детект→async buildEvent→dispatch. owner_id хранится в атомике, заполняется в setOnReady через getMe.

- [ ] **Step 1: Тест на UpdateRouter** — что `updateNewMessage` вызывает установленный webhook-хук ровно раз; при отсутствии хука не падает (как тесты connection-ready колбэка).
```cpp
TEST(UpdateRouter, WebhookHookFiresOnNewMessage) {
    tgw::auth::AuthStateManager auth; tgw::ws::UpdateRouter r(auth, "sid");
    int calls=0; r.setWebhookHook([&](const td::td_api::message&){ ++calls; });
    // скормить updateNewMessage -> calls==1; updateUserStatus -> без вызова
}
```

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.**
  - `UpdateRouter`: приватный `std::function<void(const td::td_api::message&)> webhook_hook_`; сеттер (до start); в `onUpdate` при `updateNewMessage::ID` (после существующего forward) — `if (webhook_hook_) webhook_hook_(*upd.message_);`.
  - `main.cpp` (под `if (config.webhooks_enabled)`): создать `WebhookRegistry` (S3-store с ключом-производным от session key), `dispatcher.start()`, зарезолвить owner_id в существующем `setOnReady` через `co_await`/sendOneWay getMe → сохранить в `std::atomic<int64_t> owner_id`. Установить `router.setWebhookHook([...](const message& m){ ... })`: получить chat-тип (нужен getChat или из кэша — минимально: private/broadcast определить по chat_id знаку/типу; если требуется getChat — делать в async buildEvent-обёртке), вызвать `detect`, при триггере запустить async-задачу (drogon::async_run/AsyncTask) `buildEvent(...)` → `dispatcher.dispatch(ev)`. Не блокировать поток-приёмник: хук лишь планирует задачу на loop.
  - chat_is_private/broadcast: определить через lightweight lookup (getChat кэш) внутри async части до detect ИЛИ передать detect минимально и уточнить в async. Реши: перенеси detect в async-обёртку, где доступен co_await getChat (тогда chat-тип точный). Хук просто планирует async-обработчик с копией нужных полей message (id, chat_id) + сам message. ВНИМАНИЕ на время жизни message: `upd.message_` принадлежит апдейту; для async скопируй нужное или передай владение (перемести проекцию/поля до планирования). Безопаснее: в хуке сразу построить лёгкий снимок (chat_id, message_id, is_outgoing, contains_unread_mention, sender, reply_to-наличие) ИЛИ сериализовать message в собственную структуру. Сформулируй владение явно и протестируй под ASan.
  - gating: если `!config.webhooks_enabled` — хук не ставится, ноль оверхеда.

- [ ] **Step 4: Прогон — PASS (dev-debug + asan + tsan).**

- [ ] **Step 5: Commit** `feat(webhook): wire detection/build/dispatch into update router`

---

### Task 9: Документация и метрики-экспозиция

**Files:**
- Modify: `README.md` (env-таблица), `deploy/helm/telegram-rest-gateway/values.yaml` (config-комментарии), `docs/openapi.yaml` (эндпоинты /v1/webhooks), `src/http/metrics_routes.cpp` (если метрики отдаются явно — сверить, что новые счётчики попадают в /metrics)

**Interfaces:** нет нового кода логики.

- [ ] **Step 1: README env-таблица** — `TGW_WEBHOOKS_ENABLED`, `TGW_WEBHOOK_TIMEOUT_MS`, `TGW_WEBHOOK_QUEUE_MAX`, `TGW_WEBHOOK_SSRF_GUARD` с дефолтами и назначением.
- [ ] **Step 2: helm values** — закомментированные примеры в блоке `config:`.
- [ ] **Step 3: docs/openapi.yaml** — задокументировать `POST/GET /v1/webhooks`, `DELETE /v1/webhooks/{id}` (admin scope, тело, ответы 200/400/401/404). Сверить, что spectral не даёт новых ошибок.
- [ ] **Step 4: Проверить /metrics** — новые счётчики экспонируются (если metrics_routes перечисляет счётчики явно — добавить; если авто — ничего).
- [ ] **Step 5: Commit** `docs: document webhook feature (env, openapi, helm)`

---

## Порядок и зависимости

1 (config) → 2 (detector) → 3 (dto projection) → 4 (registry) → 5 (context builder, зависит от 3) → 6 (dispatcher, зависит от 4) → 7 (REST, зависит от 4) → 8 (интеграция, зависит от 2/5/6/4) → 9 (доки).

Задачи 2, 3, 4 независимы между собой (после 1) — но исполняем последовательно per SDD.

## Верификация каждой задачи

Контроллер после каждой задачи: локальная arm64-сборка + затронутые тесты
(`dev-debug`; для 5/6/8 — дополнительно `tsan`), затем пуш и CI. Финал — полный CI зелёный + whole-branch review.
