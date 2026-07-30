# Humanize Typing Pause — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Перед отправкой текстового сообщения (`POST /v1/chats/{chatId}/messages`) показать индикатор «печатает…», выдержать паузу пропорционально длине текста, затем отправить; при возможности дождаться настоящего id сообщения (вместо временного) в отведённое окно.

**Architecture:** Чистая функция расчёта паузы (testable без сети) → цикл `sendChatAction`+`drogon::sleepCoro` внутри существующего корутинного паттерна хендлера → `sendMessage` → новый компонент `MessageSendTracker` (структурная калька `CorrelationMap`/`RequestState` моста, но ключ — `old_message_id`, а не `request_id`) резолвится из `UpdateRouter::onUpdate` на `updateMessageSendSucceeded/Failed`. Плюс явная настройка серверных таймаутов (Drogon + ingress) и fail-fast проверка их совместимости с бюджетом паузы.

**Tech Stack:** C++20, Drogon (корутины, `drogon::sleepCoro`), TDLib, gtest.

## Global Constraints

- Рабочая директория: git worktree `/Users/moveeeax/Public/github/telegram-rest-gateway-humanize`, ветка `feat/humanize-typing`. Только там.
- Комментарии по-русски, стиль репо. Эталоны: `src/bridge/correlation_map.{hpp,cpp}` + `src/bridge/request_state.hpp` + `src/bridge/td_bridge.{hpp,cpp}` (TdAwaitable) — структурный образец для `MessageSendTracker`; `src/auth/s3_session.cpp` (`runEvery`) — образец периодического таймера; `src/http/message_routes.cpp:142-176` (`/v1/chats` warmup) — образец многошаговой корутины `-> drogon::AsyncTask` внутри обычного `registerHandler`.
- Сборка с `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- Коммиты: conventional commits (`feat(...)`, `test(...)`, `docs(...)`). СТРОГО ЗАПРЕЩЕНЫ ИИ-трейлеры.
- Имплементер НЕ собирает локально (нет тулчейна) — контроллер прогоняет нативную arm64-сборку (`docker.io/resert/telegram-rest-gateway:builder-arm64`, `--security-opt seccomp=unconfined` для TSan) и CI после пуша. Обязателен тщательный статический self-review + сверка td_api-имён по `<td/telegram/td_api.h>` в builder-образе, если есть сомнения.
- `MessageSendTracker` — кросс-поточный компонент (резолв из потока-приёмника TDLib, ожидание из корутины HTTP-хендлера на IO-петле Drogon) — обязан быть TSan-чистым.
- Спека: `docs/superpowers/specs/2026-07-30-humanize-typing-design.md`.

Точные сигнатуры существующего кода, на которые опирается план (не выдумывай другие):
```cpp
// src/http/http_helpers.hpp
using HttpCallback = std::function<void(const drogon::HttpResponsePtr&)>;
inline drogon::HttpResponsePtr jsonResponse(Json::Value body, drogon::HttpStatusCode code);
inline drogon::HttpResponsePtr serviceError(const std::string& code, const std::string& message, drogon::HttpStatusCode code);
inline drogon::HttpResponsePtr telegramError(const td::td_api::error& error);

// src/bridge/expect.hpp
template <class T> struct Expected { object_ptr<T> value; object_ptr<error> error; bool ok() const; };
template <class T> Expected<T> expect(object_ptr<Object> object);

// src/bridge/td_bridge.hpp
TdAwaitable invoke(std::int32_t client_id, object_ptr<Function> fn);   // co_await
void sendOneWay(std::int32_t client_id, object_ptr<Function> fn);     // fire-and-forget

// src/dto/message_dto.hpp
Json::Value toJson(const td::td_api::message& message);  // id/chat_id/date/is_outgoing/content/reactions

// drogon/utils/coroutine.h (проверено в builder-образе)
internal::TimerAwaiter drogon::sleepCoro(trantor::EventLoop* loop, double delay_seconds);  // co_await

// td_api.h (проверено в builder-образе)
class sendChatAction : public Function {
    int53 chat_id_; object_ptr<MessageTopic> topic_id_; string business_connection_id_;
    object_ptr<ChatAction> action_;
    sendChatAction(int53, object_ptr<MessageTopic>&&, string const&, object_ptr<ChatAction>&&);
};
// вызов: make_object<sendChatAction>(chat_id, nullptr, "", make_object<chatActionTyping>())
```

---

### Task 1: Конфиг-флаги

**Files:**
- Modify: `src/config/config.hpp`, `src/config/config.cpp`
- Test: `tests/config_test.cpp`

**Interfaces:**
- Produces: поля `Config`:
```cpp
bool humanize_typing = false;
int humanize_chars_per_minute = 200;
int humanize_jitter_percent = 20;
int humanize_min_delay_ms = 1000;
int humanize_max_delay_ms = 10000;
int humanize_id_wait_ms = 4000;
int idle_connection_timeout_seconds = 90;
```

- [ ] **Step 1: Тесты парсинга** (по образцу `WebhookFlagsParsing`/`KeepOnlineFlagParsing` — используй РЕАЛЬНЫЙ env-хелпер фикстуры из файла, не выдумывай):
```cpp
TEST_F(ConfigTest, HumanizeTypingFlagsParsing) {
    setenv("TGW_HUMANIZE_TYPING", "true", 1);
    setenv("TGW_HUMANIZE_CHARS_PER_MINUTE", "150", 1);
    setenv("TGW_HUMANIZE_JITTER_PERCENT", "10", 1);
    setenv("TGW_HUMANIZE_MIN_DELAY_MS", "500", 1);
    setenv("TGW_HUMANIZE_MAX_DELAY_MS", "8000", 1);
    setenv("TGW_HUMANIZE_ID_WAIT_MS", "3000", 1);
    setenv("TGW_IDLE_CONNECTION_TIMEOUT_SECONDS", "120", 1);
    const auto c = tgw::config::loadConfig();
    EXPECT_TRUE(c.humanize_typing);
    EXPECT_EQ(c.humanize_chars_per_minute, 150);
    EXPECT_EQ(c.humanize_jitter_percent, 10);
    EXPECT_EQ(c.humanize_min_delay_ms, 500);
    EXPECT_EQ(c.humanize_max_delay_ms, 8000);
    EXPECT_EQ(c.humanize_id_wait_ms, 3000);
    EXPECT_EQ(c.idle_connection_timeout_seconds, 120);
}
TEST_F(ConfigTest, HumanizeTypingDefaults) {
    unsetenv("TGW_HUMANIZE_TYPING");
    const auto c = tgw::config::loadConfig();
    EXPECT_FALSE(c.humanize_typing);
    EXPECT_EQ(c.humanize_chars_per_minute, 200);
    EXPECT_EQ(c.humanize_max_delay_ms, 10000);
    EXPECT_EQ(c.humanize_id_wait_ms, 4000);
    EXPECT_EQ(c.idle_connection_timeout_seconds, 90);
}
// Fail-fast guard: см. Step 3 ниже — тест на отказ старта при несовместимых значениях.
TEST_F(ConfigTest, HumanizeTimeoutBudgetExceedsIdleTimeoutThrows) {
    setenv("TGW_HUMANIZE_MAX_DELAY_MS", "50000", 1);
    setenv("TGW_HUMANIZE_ID_WAIT_MS", "20000", 1);
    setenv("TGW_IDLE_CONNECTION_TIMEOUT_SECONDS", "60", 1);  // 70с бюджет > 60с таймаут
    EXPECT_THROW(tgw::config::loadConfig(), std::runtime_error);
}
```
Добавь все 7 новых env-переменных в фикстуру очистки окружения (как `kEnvBases`/аналог).

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.** Поля из Interfaces. Парсинг bool/int по существующему паттерну (`parseNumericEnv<int>`). Затем **fail-fast guard** в конце `loadConfig()` (после всех присвоений):
```cpp
constexpr int kHumanizeSafetyMarginMs = 2000;
if (c.humanize_max_delay_ms + c.humanize_id_wait_ms + kHumanizeSafetyMarginMs >
    c.idle_connection_timeout_seconds * 1000) {
    throw std::runtime_error(
        "config: TGW_HUMANIZE_MAX_DELAY_MS + TGW_HUMANIZE_ID_WAIT_MS (+"
        " запас 2000мс) превышает TGW_IDLE_CONNECTION_TIMEOUT_SECONDS — "
        "увеличьте таймаут или уменьшите паузу/окно ожидания");
}
```
Сверь, что `loadConfig()` уже кидает `std::runtime_error` в других местах (required-переменные) — используй тот же тип исключения, тот же стиль сообщения.

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Commit** `git add src/config tests/config_test.cpp && git commit -m "feat(config): humanize-typing flags and idle-timeout budget guard"`

---

### Task 2: Чистая функция расчёта паузы

**Files:**
- Create: `src/http/typing_delay.hpp` (header-only, по образцу `src/http/byte_range.hpp`)
- Test: `tests/typing_delay_test.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: ничего (чистая функция).
- Produces:
```cpp
namespace tgw::http {
struct TypingDelayParams {
    int chars_per_minute; int jitter_percent; int min_delay_ms; int max_delay_ms;
};
// text_length — количество UTF-8 кодпоинтов (не байт). jitter_sample — [0.0, 1.0),
// подставляется вызывающим (в проде — из RNG, в тестах — фиксированное значение).
std::chrono::milliseconds computeTypingDelay(std::size_t text_length,
                                             const TypingDelayParams& params,
                                             double jitter_sample);
}
```
Формула: `base_ms = text_length / (chars_per_minute / 60000.0)`; `factor = (1 - jitter) + jitter_sample * 2*jitter` (где `jitter = jitter_percent/100.0`) — при `jitter_sample=0.0` даёт нижнюю границу разброса `1-jitter`, при `jitter_sample=1.0` — верхнюю `1+jitter`, при `0.5` — ровно `base_ms`; `delay = clamp(base_ms*factor, min_delay_ms, max_delay_ms)`.

- [ ] **Step 1: Тесты.**
```cpp
TEST(TypingDelay, MidJitterSampleGivesBaseValue) {
    tgw::http::TypingDelayParams p{200, 20, 1000, 10000};
    // 200 симв/мин = 1 символ/300мс; 60 символов -> 18000мс -> clamp к max 10000
    auto d = tgw::http::computeTypingDelay(60, p, 0.5);
    EXPECT_EQ(d.count(), 10000);
}
TEST(TypingDelay, ShortTextClampsToMin) {
    tgw::http::TypingDelayParams p{200, 20, 1000, 10000};
    auto d = tgw::http::computeTypingDelay(1, p, 0.5);
    EXPECT_EQ(d.count(), 1000);
}
TEST(TypingDelay, JitterSampleZeroGivesLowerBound) {
    tgw::http::TypingDelayParams p{6000, 0, 0, 100000};  // 6000/мин = 100/сек = 10мс/символ
    auto low = tgw::http::computeTypingDelay(100, p, 0.0);   // base=1000мс, jitter=0 -> всегда 1000
    EXPECT_EQ(low.count(), 1000);
}
TEST(TypingDelay, JitterWidensRange) {
    tgw::http::TypingDelayParams p{6000, 20, 0, 100000};  // base для 100 символов = 1000мс
    auto low = tgw::http::computeTypingDelay(100, p, 0.0);
    auto high = tgw::http::computeTypingDelay(100, p, 1.0);
    EXPECT_EQ(low.count(), 800);   // 1000*(1-0.2)
    EXPECT_EQ(high.count(), 1200); // 1000*(1+0.2)
}
TEST(TypingDelay, ZeroLengthGivesMin) {
    tgw::http::TypingDelayParams p{200, 20, 1000, 10000};
    EXPECT_EQ(tgw::http::computeTypingDelay(0, p, 0.5).count(), 1000);
}
```

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация** `typing_delay.hpp` по формуле выше. `double` арифметика для base_ms, `clamp` через `std::clamp` с приведением к `std::chrono::milliseconds` в конце (единственная точка округления — `static_cast<long long>`).

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Регистрация в tests/CMakeLists.txt + commit** `feat(http): pure typing-delay calculation with jitter and bounds`

---

### Task 3: MessageSendTracker

**Files:**
- Create: `src/bridge/message_send_tracker.hpp`, `src/bridge/message_send_tracker.cpp`
- Test: `tests/message_send_tracker_test.cpp`
- Modify: `CMakeLists.txt` (добавить `.cpp` в `tgw_bridge`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Json::Value` (уже спроецированное сообщение), `trantor::EventLoop`.
- Produces (ОКОНЧАТЕЛЬНОЕ решение — `td::td_api::object_ptr<T>` в этой кодовой базе move-only и
  нигде не хранится/не передаётся через границы async-компонентов; по тому же принципу, что
  `ContextBuilder` в фиче вебхуков проецирует `message` в `Json::Value` СРАЗУ, а не хранит
  TDLib-объект — трекер работает только с уже спроецированными данными, полностью независим от
  td_api-типов):
```cpp
namespace tgw::bridge {

// Исход отправки, полученный из updateMessageSendSucceeded/Failed.
struct MessageSendOutcome {
    bool succeeded = false;
    Json::Value message;         // dto::toJson(*upd.message_), если succeeded == true
    std::int32_t error_code = 0;      // upd.error_->code_, если succeeded == false
    std::string error_message;        // upd.error_->message_, если succeeded == false
};

// Резолвит РОВНО ОДНА сторона — либо resolveSucceeded/resolveFailed (поток-приёмник), либо
// истечение таймаута (петля вызывающего) — атомарный find+erase под общим мьютексом, как в
// CorrelationMap (src/bridge/correlation_map.cpp).
class MessageSendTracker {
   public:
    class Awaitable {
       public:
        Awaitable(MessageSendTracker& tracker, std::int64_t old_message_id,
                  std::chrono::milliseconds timeout);
        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle);
        // nullopt — таймаут истёк без резолва.
        std::optional<MessageSendOutcome> await_resume();
       private:
        MessageSendTracker& tracker_;
        std::int64_t old_message_id_;
        std::chrono::milliseconds timeout_;
        std::shared_ptr<struct SendWaitState> state_;
    };

    Awaitable waitFor(std::int64_t old_message_id, std::chrono::milliseconds timeout) {
        return Awaitable(*this, old_message_id, timeout);
    }

    // Вызываются из потока-приёмника TDLib (UpdateRouter::onUpdate). No-op, если никто не
    // ждёт этот old_message_id (обычный случай при выключенном флаге — нулевые накладные
    // расходы: один промах в пустой/чужой карте).
    void resolveSucceeded(std::int64_t old_message_id, Json::Value message_json);
    void resolveFailed(std::int64_t old_message_id, std::int32_t error_code,
                       std::string error_message);

   private:
    friend class Awaitable;
    bool tryInsert(std::int64_t old_message_id, std::shared_ptr<struct SendWaitState> state);
    std::shared_ptr<struct SendWaitState> claim(std::int64_t old_message_id);

    std::mutex mutex_;
    std::unordered_map<std::int64_t, std::shared_ptr<struct SendWaitState>> map_;
};
}
```
Внутренний `SendWaitState` (в .cpp, приватная деталь):
```cpp
struct SendWaitState {
    std::mutex m;  // защищает resolved/result от гонки resolve*() vs timeout-колбэка
    bool resolved = false;
    std::coroutine_handle<> handle;
    trantor::EventLoop* loop = nullptr;
    std::optional<MessageSendOutcome> result;  // nullopt после claim() timeout-веткой
};
```
`resolveSucceeded`/`resolveFailed` — тонкие обёртки над общим приватным `resolveWith(old_message_id, MessageSendOutcome)`, которая делает `claim(old_message_id)` и, если нашла состояние, кладёт `outcome` и резюмирует хендл через `state->loop->queueInLoop(...)` (маршалинг в исходную петлю, как `TdBridge::resolve`).

- [ ] **Step 1: Тесты** (по образцу `tests/td_bridge_test.cpp` — тот же `LoopThread`-паттерн из `context_builder_test.cpp`/`td_bridge_test.cpp`: реальный `trantor::EventLoop`, никакого sleep-based ожидания, синхронизация через promise/future).
```cpp
// Скрипт: планируем корутину на loop через queueInLoop, co_await tracker.waitFor(...),
// результат снимаем через std::promise<std::optional<...>>.
TEST(MessageSendTracker, ResolvesBeforeTimeoutReturnsSucceeded) {
    // LoopThread (см. td_bridge_test.cpp) поднимает петлю; MessageSendTracker tracker;
    // На loop: запланировать корутину co_await tracker.waitFor(42, 2s), результат — в
    // std::promise<std::optional<MessageSendOutcome>>, снимаемый через future в тест-потоке.
    // Из ОТДЕЛЬНОГО потока (эмуляция приёмника TDLib), убедившись, что ожидание уже
    // зарегистрировано (например через небольшую фиксированную задержку — здесь это ДОПУСТИМО,
    // т.к. проверяем межпоточный резолв, а не внутреннюю логику корутины), вызвать
    // tracker.resolveSucceeded(42, someJsonValue).
    // Ассерт: future.get() содержит has_value()==true, outcome.succeeded==true,
    // outcome.message == someJsonValue.
}
TEST(MessageSendTracker, TimeoutReturnsNulloptWhenNoResolve) {
    // waitFor(42, 50ms) без вызова resolve*() -> nullopt после ~50мс.
}
TEST(MessageSendTracker, ResolveFailedReturnsErrorFields) {
    // resolveFailed(42, 400, "FAILED") до таймаута -> outcome.succeeded==false,
    // outcome.error_code==400, outcome.error_message=="FAILED".
}
TEST(MessageSendTracker, ResolveForUnknownIdIsNoOp) {
    // resolveSucceeded(999, ...) без единого waitFor -> не падает, не влияет на другие ожидания.
}
TEST(MessageSendTracker, DoubleResolveDoesNotDoubleResume) {
    // resolveSucceeded(42, ...) сразу после того, как таймаут уже сработал (короткий timeout,
    // sleep на тест-стороне чуть дольше timeout перед вызовом resolve) -> resolve — no-op,
    // ловим через флаг/счётчик, что coroutine resumed exactly once (тот же инвариант
    // "резолвит ровно одна сторона", что у CorrelationMap/TdBridge).
}
```
Резолв timeout: `await_suspend` регистрирует состояние в `map_` (под мьютексом `MessageSendTracker::mutex_`, как `CorrelationMap::tryInsert`), запоминает `loop = trantor::EventLoop::getEventLoopOfCurrentThread()` (fallback `drogon::app().getLoop()`, как в `TdAwaitable`), затем планирует `loop->runAfter(std::chrono::duration<double>(timeout_).count(), [this]{ /* timeout-claim */ })` — колбэк вызывает `tracker_.claim(old_message_id_)` (атомарный find+erase, ТА ЖЕ функция, что вызовет `resolve()`, если он раньше). Если `claim` вернул непустой state и `state->resolved==false` (или проще: `claim` в принципе не найдёт запись, если её уже забрал `resolve()`, — единственная линия защиты) — резюмирует хендл с `nullopt`. `resolve()` делает `claim(old_message_id)`, если нашёл — резюмирует хендл через `loop->queueInLoop(...)` с результатом (маршалинг в исходную петлю, как в `TdBridge::resolve`).

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация** по описанной механике. `tryInsert`/`claim` — буквальная структурная калька `CorrelationMap::tryInsert`/`claim` (`src/bridge/correlation_map.cpp`), но без `max_inflight`-лимита (не требуется спекой — не блокирующий ресурс, лимитируется естественным темпом HTTP-запросов).

- [ ] **Step 4: Прогон — PASS (обязательно под TSan).**

- [ ] **Step 5: CMake + commit** `feat(bridge): MessageSendTracker for old_message_id confirmation`

---

### Task 4: Подключить резолв в UpdateRouter

**Files:**
- Modify: `src/ws/update_router.hpp`, `src/ws/update_router.cpp`
- Test: `tests/update_router_test.cpp`

**Interfaces:**
- Consumes: `tgw::bridge::MessageSendTracker::resolveSucceeded/resolveFailed`.
- Produces: `void UpdateRouter::setMessageSendTracker(tgw::bridge::MessageSendTracker& tracker);` — опциональный (может быть не задан — тогда резолв не вызывается, как остальные опциональные хуки `event_publisher_`/`on_connection_ready_`).

- [ ] **Step 1: Тест.**
```cpp
TEST(UpdateRouter, MessageSendSucceededResolvesTracker) {
    // AuthStateManager auth; UpdateRouter r(auth, "sid");
    // tgw::bridge::MessageSendTracker tracker; r.setMessageSendTracker(tracker);
    // скормить updateMessageSendSucceeded{old_message_id_=42, message_=<сообщение с id=100>}
    // -> зарегистрировать tracker.waitFor(42, ...) ДО скармливания апдейта (на реальном loop,
    //    по образцу MessageSendTracker-тестов из Task 3), дождаться, что await_resume() дал
    //    MessageSendOutcome{succeeded=true, message["id"]=="100", ...}.
    // Существующий WS-форвардинг апдейта должен остаться (не удалять существующую логику
    // updateMessageSendSucceeded в buildForwardable/onUpdate!) — проверь отдельным ассертом,
    // что forwardable-путь (WS) тоже сработал как раньше.
}
TEST(UpdateRouter, MessageSendFailedResolvesTrackerWithError) {
    // updateMessageSendFailed{old_message_id_=42, error_=make_object<error>(400,"BAD")}
    // -> outcome.succeeded==false, error_code==400, error_message=="BAD".
}
TEST(UpdateRouter, MessageSendTrackerAbsentDoesNotCrash) {
    // Без setMessageSendTracker(...) — updateMessageSendSucceeded/Failed обрабатываются как
    // раньше, не падает.
}
```

- [ ] **Step 2: Прогон — FAIL.**

- [ ] **Step 3: Реализация.** В `update_router.hpp` — приватный `tgw::bridge::MessageSendTracker* message_send_tracker_ = nullptr;` + сеттер (по образцу `setOnConnectionReady`, задаётся до `start()`). В `update_router.cpp::onUpdate` — **ДОБАВИТЬ** (не заменяя существующий WS-форвард, который остаётся ниже по коду как есть) резолв ПЕРЕД веткой `buildForwardable`:
```cpp
if (message_send_tracker_ != nullptr) {
    if (update->get_id() == api::updateMessageSendSucceeded::ID) {
        const auto& upd = static_cast<const api::updateMessageSendSucceeded&>(*update);
        if (upd.message_ != nullptr) {
            message_send_tracker_->resolveSucceeded(upd.old_message_id_,
                                                     tgw::dto::toJson(*upd.message_));
        }
    } else if (update->get_id() == api::updateMessageSendFailed::ID) {
        const auto& upd = static_cast<const api::updateMessageSendFailed&>(*update);
        if (upd.error_ != nullptr) {
            message_send_tracker_->resolveFailed(upd.old_message_id_, upd.error_->code_,
                                                 upd.error_->message_);
        }
    }
}
// далее — существующий код без изменений: buildForwardable(*update), WS fan-out, event_publisher_.
```
`update` в этой точке — параметр `onUpdate(api::object_ptr<api::Object> update)`; не перемещай его до вызова резолва (существующий код ниже всё ещё использует `*update` для `buildForwardable`) — резолв и форвард читают один и тот же объект дважды, без move между ними.

- [ ] **Step 4: Прогон — PASS.**

- [ ] **Step 5: Commit** `feat(ws): resolve MessageSendTracker on updateMessageSendSucceeded/Failed`

---

### Task 5: Переписать хендлер sendMessage

**Files:**
- Modify: `src/http/message_routes.cpp` (заменить блок `POST /v1/chats/{chatId}/messages`, строки см. текущий файл — не по номерам, ищи по комментарию `// POST /v1/chats/{chatId}/messages — отправка текста`)
- Modify: `src/http/routes.cpp` или где создаётся `MessageSendTracker`/прокидывается в `registerMessageRoutes` — сверь текущую сигнатуру `registerMessageRoutes(bridge, client_id, upload_dir)` в `main.cpp` и добавь параметры `tgw::config::Config` (или только нужные поля) + `tgw::bridge::MessageSendTracker&`.
- Test: `tests/route_smoke_test.cpp` (не ломать существующие проверки на этот путь).

**Interfaces:**
- Consumes: `computeTypingDelay` (Task 2), `MessageSendTracker::waitFor` (Task 3, с учётом финального решения по типу результата из Task 4), `drogon::sleepCoro`.

- [ ] **Step 1: Изучи ТЕКУЩИЙ хендлер целиком** (блок `POST /v1/chats/{chatId}/messages` в `message_routes.cpp`) — валидация текста, Idempotency-Key claim/release/store, `makeFormattedText`, построение `sendMessage`, `launchInvoke`. Всё ДО построения `fn` (валидация + idempotency claim) остаётся ДОСЛОВНО как есть.

- [ ] **Step 2: Замени `launchInvoke(...)` на инлайн-корутину**, встраивающую (при `config.humanize_typing`) typing-cycle и (при обоих флагах доступности) id-wait. Структура (адаптируй под финальный тип результата `MessageSendTracker` из Task 3/4):
```cpp
constexpr auto kTypingRefreshInterval = std::chrono::milliseconds(4000);

[](tgw::bridge::TdBridge& td, std::int32_t cid, std::int64_t chat_id,
   api::object_ptr<api::inputMessageReplyToMessage> reply_to,
   api::object_ptr<api::formattedText> formatted, std::size_t text_length,
   std::string idem_key, bool humanize_enabled,
   tgw::http::TypingDelayParams delay_params, int id_wait_ms,
   tgw::bridge::MessageSendTracker& tracker,
   HttpCallback cb) -> drogon::AsyncTask {
    trantor::EventLoop* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (loop == nullptr) {
        loop = drogon::app().getLoop();
    }
    if (humanize_enabled) {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        auto remaining = tgw::http::computeTypingDelay(text_length, delay_params, uni(rng));
        while (remaining.count() > 0) {
            td.sendOneWay(cid, api::make_object<api::sendChatAction>(
                                   chat_id, nullptr, "", api::make_object<api::chatActionTyping>()));
            const auto tick = std::min(remaining, kTypingRefreshInterval);
            co_await drogon::sleepCoro(loop, std::chrono::duration<double>(tick).count());
            remaining -= tick;
        }
    }

    auto fn = api::make_object<api::sendMessage>();
    fn->chat_id_ = chat_id;
    fn->reply_to_ = std::move(reply_to);
    auto content = api::make_object<api::inputMessageText>();
    content->text_ = std::move(formatted);
    fn->input_message_content_ = std::move(content);

    auto obj = co_await td.invoke(cid, std::move(fn));
    auto message = tgw::bridge::expect<api::message>(std::move(obj));
    if (!message.ok()) {
        if (!idem_key.empty()) {
            IdempotencyCache::instance().release(idem_key);
        }
        cb(telegramError(*message.error));
        co_return;
    }
    const std::int64_t temp_id = message.value->id_;
    const std::int64_t out_chat_id = message.value->chat_id_;

    if (humanize_enabled && id_wait_ms > 0) {
        auto confirmed = co_await tracker.waitFor(temp_id, std::chrono::milliseconds(id_wait_ms));
        if (confirmed.has_value()) {
            if (confirmed->succeeded) {
                Json::Value data = confirmed->message;  // уже спроецировано (dto::toJson)
                data["sending_state"] = "sent";
                Json::Value body;
                body["ok"] = true;
                body["data"] = data;
                auto resp = jsonResponse(std::move(body), drogon::k200OK);
                if (!idem_key.empty()) {
                    IdempotencyCache::instance().store(idem_key, resp->statusCode(),
                                                       std::string(resp->body()));
                }
                cb(resp);
            } else {
                if (!idem_key.empty()) {
                    IdempotencyCache::instance().release(idem_key);
                }
                // Собери HttpResponsePtr тем же путём, что telegramError(const error&) —
                // либо сконструируй временный td_api::error(confirmed->error_code,
                // confirmed->error_message) и передай в telegramError(*tmp), либо заведи
                // локальный хелпер с той же логикой httpStatusForTdError на паре (code,message).
                // Выбери вариант с временным error-объектом — минимальный дифф, переиспользует
                // существующий telegramError() без дублирования маппинга статусов.
                auto tmp_error = api::make_object<api::error>(confirmed->error_code,
                                                              confirmed->error_message);
                cb(telegramError(*tmp_error));
            }
            co_return;
        }
        // nullopt -> таймаут, падаем в pending-ответ ниже как раньше
    }

    Json::Value data;
    data["temporary_message_id"] = std::to_string(temp_id);
    data["chat_id"] = std::to_string(out_chat_id);
    data["sending_state"] = "pending";
    Json::Value body;
    body["ok"] = true;
    body["data"] = data;
    auto resp = jsonResponse(std::move(body), drogon::k202Accepted);
    if (!idem_key.empty()) {
        IdempotencyCache::instance().store(idem_key, resp->statusCode(), std::string(resp->body()));
    }
    cb(resp);
    co_return;
}(bridge, client_id, chatId, std::move(fn->reply_to_), std::move(content->text_) /* уже перемещён в fn выше — НЕ строй content/fn заранее как в старом коде, перенеси их построение ВНУТРЬ корутины по образцу выше */,
  (*json)["text"].asString().size() /* УТОЧНИ: длина в UTF-8 кодпоинтах, не байтах — найди
  существующий helper подсчёта кодпоинтов в проекте (grep по utf8/codepoint/wchar в src/), если
  нет — считай по байтам с явным комментарием об ограничении (ASCII-приближение), не блокируй
  задачу на этом, но зафиксируй как отдельную мелкую находку в отчёте */,
  idem_key, config.humanize_typing,
  tgw::http::TypingDelayParams{config.humanize_chars_per_minute, config.humanize_jitter_percent,
                               config.humanize_min_delay_ms, config.humanize_max_delay_ms},
  config.humanize_id_wait_ms, tracker, std::move(cb));
```
`TypingDelayParams` строится прямо в точке вызова корутины (последний аргумент перед `id_wait_ms`) — отдельная именованная переменная не нужна.

**КРИТИЧНО:** Валидация текста и построение `formatted`/`reply_to` (через `makeFormattedText`, парсинг `reply_to_message_id`) остаются на своих местах — ДО входа в корутину (синхронная часть хендлера не меняется), только сам вызов `sendMessage`+ответ уезжает внутрь корутины вместо `launchInvoke`. Перечитай псевдокод выше внимательно и подгони под реальные имена локальных переменных существующего хендлера (`formatted`, `content`, `fn`, `replyTo`) — не изобретай новые имена без причины.

`config`/`delay_params`/`tracker` должны быть доступны в замыкании `registerHandler`-лямбды верхнего уровня — проверь, как сейчас `bridge`/`client_id` захватываются в `[&bridge, client_id]`, и добавь туда же `&config` (или конкретные поля) и `&tracker` по той же схеме (ссылки на объекты, живущие в `main()` дольше приложения).

- [ ] **Step 2: Прогон — компиляция + существующие тесты route-smoke на этот путь (проверь `route_smoke_test.cpp` — есть ли там прямая проверка `/v1/chats/{chatId}/messages`; если нет узкого юнита на тело ответа — это нормально, полагаемся на существующее покрытие фильтра/скоупа).**

- [ ] **Step 3: Добавь узкий юнит на подсчёт длины текста** (если использовал байтовое приближение — тест, фиксирующий ЭТО поведение явно, чтобы будущая правка не сломала его молча):
```cpp
// В существующем файле с тестами message_routes, если такой есть, либо новый минимальный тест
// на длину, если решение по Step 2 использовало приближение — задокументируй choice в тесте.
```

- [ ] **Step 4: Commit** `feat(http): humanized typing pause and real-id wait for sendMessage`

---

### Task 6: Явные серверные таймауты + fail-fast

**Files:**
- Modify: `src/main.cpp`, `deploy/helm/telegram-rest-gateway/values.yaml`, `deploy/helm/telegram-rest-gateway/templates/ingress.yaml` (если аннотации не параметризованы — сверь текущий шаблон)

**Interfaces:**
- Consumes: `config.idle_connection_timeout_seconds` (Task 1).

- [ ] **Step 1: main.cpp** — добавь явный вызов (рядом с другими `drogon::app().set...` вызовами, найди их существующее место):
```cpp
drogon::app().setIdleConnectionTimeout(static_cast<std::size_t>(config.idle_connection_timeout_seconds));
```
Сверь точную сигнатуру `setIdleConnectionTimeout` в `drogon/HttpAppFramework.h` (тип параметра — `size_t` секунд, по `config.example.yaml`, который уже сверен) — если тип другой, используй реальный.

- [ ] **Step 2: values.yaml** — добавь в блок `ingress.annotations` (или в комментарий-пример, если сейчас список аннотаций жёстко задан в values, а не темплейте) дефолтные `proxy-read-timeout`/`proxy-send-timeout`, согласованные со значением `TGW_IDLE_CONNECTION_TIMEOUT_SECONDS` (default `90`, как в Task 1) — по образцу того, как это уже сделано в `deploy/helm/telegram-rest-gateway-tools/values.yaml` для MCP-ingress (`nginx.ingress.kubernetes.io/proxy-read-timeout: "3600"` и т.п.). Если аннотации в гейтвей-чарте сейчас захардкожены оператором в values (не параметризованы шаблоном) — просто добавь два новых ключа со значением `"90"` в дефолтные аннотации, с комментарием на русском, откуда взято число (тот же бюджет пауз).

- [ ] **Step 3: helm lint** (у контроллера есть локальный `helm` — этот шаг проверит контроллер, не имплементер).

- [ ] **Step 4: Commit** `feat(main,helm): explicit idle-connection timeout budgeted for humanize pause`

---

### Task 7: Документация

**Files:**
- Modify: `README.md`, `docs/openapi.yaml`

- [ ] **Step 1: README** — новая секция «Имитация человеческой печати» (по образцу секции «Онлайн-статус»): что делает флаг, все 6 новых env (`TGW_HUMANIZE_TYPING`, `_CHARS_PER_MINUTE`, `_JITTER_PERCENT`, `_MIN_DELAY_MS`, `_MAX_DELAY_MS`, `_ID_WAIT_MS`) + `TGW_IDLE_CONNECTION_TIMEOUT_SECONDS` в таблице env с дефолтами (сверь по `config.cpp` факту, не по этому плану).

- [ ] **Step 2: docs/openapi.yaml** — обнови описание `POST /v1/chats/{chatId}/messages`: добавь `200` ответ (полное сообщение, `sending_state: sent`) как альтернативу существующему `202` (`sending_state: pending`), с пояснением условия (когда включён `TGW_HUMANIZE_TYPING` и апдейт успел прийти в окно `TGW_HUMANIZE_ID_WAIT_MS`).

- [ ] **Step 3: Commit** `docs: document humanize-typing feature and dual response contract`

---

## Порядок и зависимости

1 (конфиг) → 2 (формула, независима от 1 кроме имён полей) → 3 (tracker) → 4 (wiring в UpdateRouter, зависит от 3 и требует финального решения по типу результата — при необходимости корректирует Task 3 задним числом) → 5 (хендлер, зависит от 2+3+4) → 6 (таймауты, независима, может идти параллельно с 2-5) → 7 (доки, в конце).

## Верификация каждой задачи

Контроллер: локальная arm64-сборка + релевантные тесты после каждой задачи (`dev-debug`; Task 3/4 — обязательно дополнительно `tsan`), затем пуш и CI. Финал — whole-branch review по образцу предыдущих фич в этом репозитории.
