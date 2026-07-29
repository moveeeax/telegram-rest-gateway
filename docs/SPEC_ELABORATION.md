# Приложение к ТЗ: детальная проработка (v3.0, финал)

Документ дополняет и уточняет `TZ_TDLib_Drogon_REST.md` на уровне реализации. Организован по разделам исходного ТЗ; новые подразделы помечены явно. Пометки `[Факт]` / `[Решение]` / `[Открытый вопрос]` — как в ТЗ. Имена классов/методов — предложения к реализации.

Относительно черновика v2.1 проведена ревизия под принцип **«MVP без золочения»**: переусложнённые механизмы упрощены или отложены (помечены `post-MVP`), пробелы жизненного цикла и конкурентности — закрыты.

> **Сквозная терминология:** в эскизе 5.4 `td::Task` → `drogon::Task`. Мост живёт в `tgw::bridge`, DTO — в `tgw::dto`. Ниже `Task<T>` == `drogon::Task<T>`.

---

## 5. Мост «async TDLib → request/response HTTP» (ядро)

### 5.2′ Механизм корреляции

`[Факт]` `td::ClientManager::send()` потокобезопасен и вызывается из любого потока (в т.ч. из event-loop'ов Drogon); сериализуется только `receive()` (ровно один поток). `request_id` несёт только уникальность.

`[Факт]` `td::ClientManager::Response{ ClientId client_id; RequestId request_id; td_api::object_ptr<td_api::Object> object; }`. При штатном таймауте `receive(timeout)` возвращает `Response` с `object == nullptr`. `object` для ЛЮБОГО запроса может оказаться `td_api::error`.

`[Решение]` Генератор id:
```cpp
uint64_t RequestIdGenerator::next() {
    uint64_t id = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    while (id == 0) id = counter_.fetch_add(1, std::memory_order_relaxed) + 1; // guard от wrap
    return id;
}
```
Инвариант: id уникален на всё время жизни записи; переиспользование до `erase` исключено.

### 5.6 Владение состоянием и единоличный резолв *(новый)*

`[Решение]` Разделяемое состояние вызова, которым ОДНОВРЕМЕННО владеют запись в `CorrelationMap` и awaiter:
```cpp
struct RequestState {
    std::mutex m;
    enum class Phase : uint8_t { Pending, Fulfilled, TimedOut, ShuttingDown };
    Phase phase = Phase::Pending;
    td_api::object_ptr<td_api::Object> result;
    std::coroutine_handle<> handle;
    trantor::EventLoop* loop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    uint64_t request_id = 0;
};
using RequestStatePtr = std::shared_ptr<RequestState>;
```
- `CorrelationMap` = `std::unordered_map<uint64_t, RequestStatePtr>` под `std::mutex map_mutex_`.
- **Правило единоличного резолва:** поток-приёмник, TTL-сборщик и shutdown делают под `map_mutex_` атомарную операцию `find + erase`. Кто извлёк узел — тот владеет резолвом; второй уже ничего не найдёт. Затем владелец под `RequestState::m` делает CAS фазы `Pending → *`; если фаза уже не `Pending` — резолв не выполняется (двойная защита).
- **Порядок обязателен:** `erase` из map и CAS фазы относятся к одному узлу и выполняются одним владельцем последовательно; никакая другая сторона узел уже не видит (он извлечён из map). Гонок между `erase` и CAS нет по построению.
- Сам резолв (запись `result` + `resume`) выполняется ВНЕ `map_mutex_`, через `loop->queueInLoop`, где `RequestStatePtr` захвачен в лямбду по значению (гарантирует жизнь состояния на время resume).
- UAF исключён: кадр корутины Drogon не разрушается досрочно (даже при разрыве HTTP-клиента), а `RequestState` живёт по shared_ptr, пока держится хоть одной стороной.

### 5.7 Порядок в await_suspend: insert-before-send *(новый)*

`[Решение]` `send()` И вставку в map выполнять ВНУТРИ `TdAwaitable::await_suspend(h)` строго в этом порядке (устраняет гонку «resume до установки handle» и «потерянный ответ до регистрации»):
```cpp
bool await_suspend(std::coroutine_handle<> h) {
    state_->handle = h;
    state_->loop = trantor::EventLoop::getEventLoopOfCurrentThread(); // фолбэк: drogon::app().getLoop()
    state_->deadline = clock_() + ttl_;
    { std::lock_guard lk(map_mutex_); corr_map_[state_->request_id] = state_; } // становится резолвабельной
    transport_.send(client_id_, state_->request_id, std::move(fn_));         // ТОЛЬКО теперь
    return true;
}
```
Реентерабельности нет: resume приходит `queueInLoop` на тот же loop, где выполняется `await_suspend`, и обработается только после возврата из него.

### 5.8 Поток-приёмник (ReceiveLoop) *(уточнение 5.3)*

`[Решение]`
```cpp
void ReceiveLoop::run() {
    while (!stopping_.load(std::memory_order_relaxed)) {
        receive_iterations_.fetch_add(1, std::memory_order_relaxed);   // heartbeat
        last_iteration_.store(steadyNow());
        auto resp = transport_.receive(td_receive_timeout_seconds_);   // default 1.0с
        if (resp.object == nullptr) continue;                          // штатный таймаут
        try { dispatch(resp); } catch (...) { /* лог без контента, метрика, продолжить */ }
    }
}
```
- `TdBridge::dispatch(const Response&)` — чистая функция (для синхронного unit-теста без реального `receive`): ветвление `request_id != 0` (ответ → claim из map → `queueInLoop` резолв) / `== 0` (апдейт → Update Router, см. 6.3′) / нет записи (лог+drop, метрика `response_after_ttl_total`).
- Поток-приёмник НИКОГДА не вызывает `handle.resume()` напрямую и не делает тяжёлой работы (маппинг DTO/JSON — уже в loop Drogon). `object_ptr` передаётся во внешний loop только через `std::move` в init-capture.
- `td_receive_timeout_seconds` default 1.0 → отзывчивость shutdown ≤ 1с, без busy-loop.

### 5.9 Синхронные методы и fire-and-forget *(замена 5.5)*

`[Факт]` В `td_api` практически ВСЕ async-функции возвращают ровно один результат (типизированный или `error`), включая `close`/`setLogVerbosityLevel`. `td_api.tl` не помечает функции как «без ответа». Реальное различие — набор СИНХРОННЫХ функций (пометка «Can be called synchronously»): `getTextEntities`, `parseTextEntities`, `getFileMimeType`, `getLogVerbosityLevel/setLogVerbosityLevel`, `getJsonValue` и т.п.

`[Решение]`
- Убрать концепцию «whitelist fire-and-forget по td_api.tl». ВСЕ async-запросы с `request_id != 0` регистрировать в CorrelationMap; TTL спасает аномалии.
- Синхронные методы вызывать через статический `td::ClientManager::execute(fn)` (немедленный результат, не проходит через `receive`).
- API моста: `Task<object_ptr<Object>> invoke(...)` (ожидает ответ) и `void sendOneWay(...)` — только для явного fire-and-forget `close` при shutdown.

### 5.10 Проверка типа ответа *(новый)*

`[Решение]` Helper на стороне хендлера:
```cpp
template<class T> Result<T> expect(td_api::object_ptr<td_api::Object>&& o);
// если o->get_id()==td_api::error::ID → вернуть td_api::error (маппинг конверта §9),
// иначе td::move_tl_object_as<T>(std::move(o))
```
Мост возвращает `object_ptr<Object>`; `expect<T>` вызывает хендлер. Статический даункаст `error` к ожидаемому типу без проверки = UB.

### 5.11 TTL-сборщик, лимит in-flight, время *(уточнение 5.5)*

`[Решение]`
- `correlation_ttl_ms` = 30000, `correlation_sweep_ms` = 1000 (конфиг). При истечении — резолв `ServiceError{504, "UPSTREAM_TIMEOUT"}`.
- Инъекция времени: `using SteadyClock = std::function<std::chrono::steady_clock::time_point()>`; `CorrelationMap(clock)`, метод `sweepExpired(now)` вызывается и таймером Drogon, и тестом вручную (FakeClock).
- MVP: полный скан map под коротким локом допустим. `[Открытый вопрос]` при `in-flight > 10k` перейти на вторичный индекс по дедлайну (`multimap<time_point, request_id>`). Метрика `correlation_pending`.
- `max_inflight_requests` (default 2000): при достижении новый хендлер сразу `503 SERVICE_BUSY`, не создавая запись и не вызывая `send()`. Atomic-счётчик inc при insert / dec при erase. Метрика `inflight_requests`.

### 5.12 Тестируемость: порт транспорта *(новый, фундамент)*

`[Решение]` Обязательный seam:
```cpp
struct ITdTransport {
    virtual int32_t createClientId() = 0;
    virtual void send(int32_t cid, uint64_t rid, td_api::object_ptr<td_api::Function>) = 0;
    virtual Response receive(double timeout) = 0;
    virtual ~ITdTransport() = default;
};
```
`RealTdTransport` оборачивает `td::ClientManager` 1:1. `FakeTdTransport` — потокобезопасные scripted-очереди (`pushResponse(rid,obj)`, `pushUpdate(obj)`). `TdBridge` конструируется от `ITdTransport&`. Это делает возможными unit/TSan-тесты моста без линковки TDLib.

### 5.13 Семантика отмены HTTP-клиента

`[Факт]` Разрыв HTTP-клиента в Drogon НЕ отменяет и не разрушает кадр корутины-хендлера: `co_await` возобновится (по ответу или TTL-504), сформированный `HttpResponse` отбросится. Сайд-эффект TDLib (напр. `sendMessage`) всё равно произойдёт — семантика **at-least-once**, ресурс ограничен TTL. Не имитировать cancellation через сырые хуки Drogon. Метрика (диагностическая, post-MVP) `orphaned_after_disconnect`.

---

## 6. Сессия и апдейты

### 6.3′ Update Router: fan-out напрямую, реестр, порядок, back-pressure *(упрощено)*

`[Решение]` **Без промежуточной очереди.** Убрана глобальная `max_update_queue` с drop-oldest: апдейты уже сериализованы receive-потоком, второй конвейер избыточен. Fan-out выполняется ПРЯМО в `dispatch()`: для апдейта receive-поток берёт snapshot подписчиков и раскидывает через `queueInLoop` на loop каждого соединения. Настоящий предохранитель — per-connection back-pressure (ниже). Глобальную очередь — `post-MVP`, если профиль покажет, что fan-out тормозит receive.

`[Решение]` **Реестр WS-подписчиков** `WsSubscriberRegistry` (упрощено, без COW): `std::vector<WebSocketConnectionPtr>` под обычным `std::mutex`. `connect`/`disconnect`/`fan-out` берут короткий lock; копия вектора под локом дешёвая (подписчиков единицы). Регистрация в `handleNewConnection`, снятие в `handleConnectionClosed`. Per-connection состояние — через `conn->setContext(std::make_shared<WsCtx>())` (`session_id`, `seq`, `pending_updates`, `ws_conn_id`). COW-снапшот вернуть только при доказанном contention (`post-MVP`).

`[Факт]` `trantor::TcpConnection::send` потокобезопасен (сам ставит в очередь loop). `[Решение]` Для строгого порядка и учёта back-pressure отправку маршалить через `conn->getLoop()->queueInLoop([conn,msg]{ if (conn->connected()) conn->send(msg); })`.

`[Решение]` **Инвариант порядка на соединении:** апдейты одной сессии сериализуются в один `queueInLoop` конкретного соединения в порядке `receive()`; trantor гарантирует FIFO задач одного loop → порядок на конкретном соединении сохраняется. Не вводить промежуточную многопоточную обработку, переупорядочивающую апдейты.

`[Решение]` **Back-pressure (один механизм):** per-connection `std::atomic<size_t> pending_updates_` (inc при enqueue, dec в колбэке завершения отправки). Двойной механизм (счётчик И `setHighWaterMarkCallback`) убран — оставлен только счётчик. При превышении `ws_max_pending_updates`=1000 — `conn->forceClose()` (WS close-code **1013** Try Again Later опционально), лог `ws_conn_id + count` (без контента). Политика MVP = **disconnect** (не drop-oldest). При `handleConnectionClosed`/`forceClose` per-connection состояние обнуляется в одном месте (снятие из реестра + сброс `pending_updates_`, см. 8.10).

### 6.4 Фильтр служебных апдейтов и OptionStore *(новый)*

`[Решение]` В Update Router — фильтр: `updateOption`/`updateConnectionState`/`updateAuthorizationState` обрабатываются ВНУТРЕННЕ (кэши `OptionStore` с `my_id`, `AuthStateHolder`, `connection_state`), по умолчанию НЕ форвардятся в WS. Наружу в MVP форвардить прикладные: `updateNewMessage`, `updateMessageContent`, `updateMessageSendSucceeded/Failed`, `updateNewChat`, `updateChatLastMessage`, `updateChatReadInbox/Outbox`, `updateFile`, `updateDeleteMessages`, `updateChatPosition`.

### 6.5 Схема WS-сообщений и keepalive *(новый)*

`[Решение]` Исходящий фрейм:
```json
{"type":"update","update_type":"updateNewMessage","seq":<uint64 per-conn>,"session_id":"default","data":<DTO>}
```
При подключении — `{"type":"hello","session_id":"default"}`. Keepalive: `conn->setPingMessage("", std::chrono::seconds(30))`. `handleNewMessage` в MVP: принимать только `{"type":"pong"}`/пустое, неизвестное — игнорировать (не падать). Фильтрация подписок — вне MVP (параметры игнорируются). `seq` — только для диагностики (at-most-once).

---

## 7. Авторизация

### 7.1 StartupBootstrapper: авто-подъём сессии *(новый, критично)*

`[Факт]` `authorization_state` начинает поступать (`updateAuthorizationState`) только ПОСЛЕ `create_client_id()` и первого обращения к клиенту. При рестарте с уже сохранённой сессией никто не вызывает `POST /v1/auth/session`, поэтому без явного бутстрапа поток апдейтов не стартует и сервис навсегда застрянет вне Ready.

`[Решение]` `StartupBootstrapper` выполняется при старте процесса **ДО** приёма HTTP:
1. `create_client_id()`.
2. Первыми вызовами: `setLogVerbosityLevel{tdlib_log_verbosity}` + `setLogStream{logStreamEmpty|logStreamFile}` (см. §13).
3. Дождаться первого `updateAuthorizationState`.
4. Если пришло `authorizationStateWaitTdlibParameters` — АВТОМАТИЧЕСКИ отправить `setTdlibParameters` из конфига (api_id/api_hash/ключ, см. 7.6), не дожидаясь HTTP.
5. Дальше автомат идёт сам: при наличии сохранённой сессии дойдёт до `Ready`; при её отсутствии остановится на `wait_phone_number` и будет ждать `POST /v1/auth/{sid}/phone`.

`[Решение]` **Инвариант старта подсистем** (порядок обязателен): receive-поток → StartupBootstrapper (до первого `updateAuthorizationState`) → затем `drogon::app().run()`/listen. Watchdog (10.5) НЕ активируется до первого успешного receive-итератора. До готовности receive-потока: `/v1/health`=200, `/v1/ready`=503 `{ready:false, state:"starting"}`, прикладные session-эндпоинты → `503 SERVICE_STARTING` (не NPE).

`[Решение]` **Идемпотентность `POST /v1/auth/session`:** если состояние уже `> WaitTdlibParameters` — вернуть текущее состояние без повторного `setTdlibParameters` (повторный вызов TDLib отверг бы с error).

### 7.2′ Автомат состояний (MVP — только phone-flow) *(упрощено)*

`[Решение]` MVP обрабатывает индивидуально ТОЛЬКО достижимые в phone-flow состояния:
`wait_tdlib_parameters`, `wait_phone_number`, `wait_code`, `wait_password`, `ready`, `logging_out`, `closing`, `closed`.

Все прочие (`wait_registration`, `wait_email_address`, `wait_email_code`) — одна ветка `default → 409 AUTH_REQUIRED` с machine-readable `state` в теле (не «молча зависнуть», но и без индивидуального маппинга/DTO-полей). Email-логин, регистрация нового номера — `post-MVP`. **Реализовано сверх плана:** QR-логин (`wait_other_device_confirmation`: `POST /v1/auth/qr`, `qr_link` в state), resend кода, `code_info` — Telegram может молча не доставлять коды кастомным api_id, QR оказался обязательным путём.

`[Решение]` DTO наружу:
- для `wait_code` отдавать `authenticationCodeInfo` (`type`/`next_type`/`length`/`timeout`);
- для `wait_password` — `has_recovery`/`hint`.

Строковый enum `AuthStateDto.state` стабилен (см. список выше + `unsupported_state` для default-ветки).

### 7.3 AuthStateManager: состояние из UPDATE + сериализация мутаций *(новый)*

`[Факт]` `authorization_state` приходит ТОЛЬКО как `updateAuthorizationState` (`request_id==0`) и ГЛОБАЛЕН (не коррелируется с `request_id`). Ответ на `setTdlibParameters`/`setAuthenticationPhoneNumber`/`checkAuthenticationCode` — просто `ok`/`error`; новое состояние из него прочитать нельзя.

`[Решение]` `AuthStateManager` (single instance на `client_id`):
- Receive-поток при каждом `updateAuthorizationState` атомарно обновляет `AuthStateHolder{std::atomic<AuthState>}` и инкрементит `generation_counter` (atomic), будит ожидающих.
- `GET /state` читает кэш.

`[Решение]` **Per-session auth-mutex (fair):** ВСЕ мутирующие эндпоинты (`/phone`, `/code`, `/password`, `/logout`, `/code/resend`) сериализуются одним mutex — глобальный автомат нельзя гнать параллельно. Внутри критической секции:
1. Снять снапшот `(state, generation)` до отправки.
2. Отправить функцию → дождаться её `ok`/`error` по `request_id`.
3. Если ожидается смена состояния — дождаться `generation > snapshot.generation` И `state != snapshot.state` (с таймаутом `auth_transition_timeout_seconds`).
4. Если операция состояние не меняет (напр. `resendAuthenticationCode`) — переход не ждать, вернуть текущее.

Ошибки авторизации (`PHONE_CODE_INVALID`) приходят как `error` на конкретный `request_id` → `400`, состояние перечитать из холдера. Это устраняет racy «ждать следующего апдейта» при конкурентных запросах: второй запрос ждёт mutex, а не чужой апдейт.

### 7.4 Терминальные состояния: три сценария Closed *(новый)*

`[Решение]` Три флага: `shutdown_initiated`, `logout_requested`, (по умолчанию оба false). В обработчике `updateAuthorizationState`:
- `Closing` → readiness NOT_READY, запросы к сессии `503 SERVICE_CLOSING`.
- `LoggingOut`:
  - `logout_requested==true` (наш `POST /logout`) — штатный локаут: readiness NOT_READY, `tgw_logout_total++`. НЕ exit. После `Closed` сервис в состоянии «нужен ре-логин» — `POST /v1/auth/session` начинает новый логин заново (session-файлы стёрты TDLib).
  - `logout_requested==false` — удалённая ревокация: `LOG_WARN`, `tgw_session_revoked_total++`, readiness NOT_READY, degraded-режим (не restart loop). `[Открытый вопрос → продукт]` degraded vs exit (рекомендация: degraded).
- `Closed`:
  - `shutdown_initiated==true` — штатный выход.
  - `logout_requested==true` — завершение штатного локаута (см. выше), НЕ авария.
  - оба false — **АВАРИЯ:** `LOG_FATAL` + flush, `tgw_tdlib_unexpected_close_total++`, немедленный `_exit(1)` (см. 7.4a). После рестарта session-файлы живы → авто-возврат в Ready через StartupBootstrapper; `client_id` после close переиспользовать нельзя.

### 7.4a Безопасный аварийный выход *(новый)*

`[Решение]` В watchdog (10.5) и в аварийном `Closed` использовать `_exit(1)` (или `std::quick_exit`), НЕ `std::exit`. `std::exit` запускает деструкторы глобалов/atexit при живых receive/loop-потоках → гонки и возможное повреждение при конкурентной записи TDLib-БД, особенно из trantor-таймера при удержанных локах. Последовательность: `LOG_FATAL` + явный flush логов + инкремент метрики → `_exit(1)` без раскрутки. TDLib-БД транзакционна и переживает abrupt-exit; корректный `close` идёт только по graceful-пути (10.4).

### 7.5 Дополнительные auth-эндпоинты (без своего rate-limit) *(упрощено)*

`[Решение]`
- `POST /v1/auth/{sid}/logout` → `logOut` (устанавливает `logout_requested=true`, см. 7.4).
- `POST /v1/auth/{sid}/code/resend` → `resendAuthenticationCode`.
- **Собственный anti-bruteforce/backoff на `/code` и `/password` — убран.** Telegram уже защищает эти операции; дублировать защиту и держать состояние на сессию не нужно. Флуд/ошибки Telegram пробрасываются как есть (429 из §9.3). Локальный лимитер — `post-MVP` (многопользовательский сценарий).
- Тела `/phone`, `/code`, `/password` — в redaction-список логов.

### 7.6 setTdlibParameters + защита от mismatch с существующей БД *(уточнение)*

`[Факт]` Сигнатура (>=1.8.x): `setTdlibParameters(use_test_dc, database_directory, files_directory, database_encryption_key, use_file_database, use_chat_info_database, use_message_database, use_secret_chats, api_id, api_hash, system_language_code, device_model, system_version, application_version)`. Цепочка: `use_message_database` требует `use_chat_info_database` требует `use_file_database`.

`[Решение]` Дефолты: `use_test_dc=false` (в интеграционных тестах true), `use_file_database=true`, `use_chat_info_database=true`, `use_message_database=true` (обязателен для истории), `use_secret_chats=false`; раздельные `database_directory=/data/session` и `files_directory=/data/files` (0700); `system_language_code="en"`, `device_model="telegram-rest-gateway"`, `system_version=""`, `application_version=<версия сервиса>`. `database_encryption_key` = 32 байта, читается из внешнего секрета (см. §13).

`[Решение]` **Mismatch-защита:** если при СУЩЕСТВУЮЩЕЙ БД `setTdlibParameters` вернул `error` (разошёлся ключ шифрования или api-креды) — `LOG_FATAL` с машинным кодом (`DB_KEY_MISMATCH` / `PARAMS_MISMATCH`), метрика, **отказ старта** (НЕ retry-loop — иначе риск перезатирания). `database_encryption_key` ДОЛЖЕН быть стабильным и внешним; **запрещено авто-генерировать его на старте** (иначе полная потеря сессии). Тест: подмена ключа → детерминированный отказ, БД не повреждена (14.20).

---

## 8. API (контракт)

### 8.0 Транспорт *(новый)*

`[Решение]` Сервис ОБЯЗАН работать за TLS-терминирующим reverse-proxy ИЛИ слушать loopback. Конфиг `listen_address` (default `127.0.0.1`), `listen_port`. Отказ старта при bind не на loopback без `behind_tls_proxy=true`/`allow_insecure_bind=true`. Клиентский IP — из `X-Forwarded-For` только если пришло от `trusted_proxies`, иначе peer-адрес.

### 8.1′ Bearer-фильтр и WS-аутентификация *(упрощено)*

`[Решение]` `BearerAuthFilter : drogon::HttpFilter<BearerAuthFilter>`. Набор токенов — из секрета (`BEARER_TOKENS_FILE`, статичный список). При старте предвычислить SHA-256 каждого токена. В `doFilter`: снять префикс `Bearer `, SHA-256 предъявленного, сравнить со ВСЕМИ хешами без early-return (`ok |= CRYPTO_memcmp(...)==0`, timing-safe). Отказ → `401` в конверте `{ok:false,error:{code:"UNAUTHENTICATED"}}`. Регистрировать на все `/v1/**` кроме `/v1/health`, `/v1/ready`. Токен не логировать даже частично. **Ротация — только через рестарт.** SIGHUP hot-reload, структурированный формат токенов `{id,label,secret,not_after}`, per-token лимиты — `post-MVP`.

`[Решение]` **WS-аутентификация — только `Sec-WebSocket-Protocol`.** В `handleNewConnection` по `HttpRequestPtr` рукопожатия читать заголовок `Sec-WebSocket-Protocol: bearer.<token>` (браузерный `WebSocket` умеет подпротокол). Долгоживущий Bearer в query — **ЗАПРЕЩЁН** (утечка в access-логи/Referer). **Ticket-flow (`POST /v1/updates/ticket` + TicketStore) — убран из MVP** (второй канал аутентификации ради гипотетических клиентов без заголовков); отложить до появления клиента, которому реально нужно. При провале — `conn->forceClose()` без апгрейда. *(Разрешает конфликт: ТЗ 8.1 допускал query-токен — отменяется.)*

### 8.2.1 Типы идентификаторов и сериализация *(новый)*

`[Факт]` В td_api chat_id/message_id/user_id — int53/int64: chat_id каналов `-100xxxxxxxxxx`, message_id = `server_id<<20`, user_id > 2^32.

`[Решение]` ВСЕ 64-битные id (chat_id, message_id, from_message_id, user_id, session_id) сериализуются и принимаются как JSON-**строки** (десятичный int64). Числами — только мелкие поля (limit, offset, progress, unix-время). Helpers: `Json::Value idToJson(int64_t){ return std::to_string(v); }`, `parseId(const std::string&)` с проверкой диапазона → `400 VALIDATION_ERROR`. В OpenAPI `type:string, pattern:'^-?[0-9]+$'`. Документировать: `message_id != Bot API id`.

### 8.2.2 Стратегия маппинга DTO *(новый)*

`[Решение]` Для MVP (~6-8 DTO) — **ручной** маппинг, без кодогена. `namespace tgw::dto`: `MessageDto`, `ChatDto`, `UserDto`, `FileDto`, `AuthStateDto`, `ErrorDto` + свободные `Json::Value toJson(const td_api::message&)` и т.д. Защита от апгрейда TDLib: (а) обращение к полям напрямую (`msg.id_`) — переименование даёт ошибку компиляции; (б) golden-тесты (JSON-снапшоты на синтетических td_api-объектах) в CI; (в) content-варианты через `switch(get_id())` с `default → {type:"unsupported"}`. Наружу отдавать только allowlist-поля.

### 8.2.3 Единый конверт ответа *(новый)*

`[Решение]` Успех: `{"ok":true,"data":<DTO|array>,"meta":{...}}`. Списки: `meta.next_cursor` (null когда данных нет), `meta.has_more`. Единый сериализатор `ApiEnvelope::success()/error()`. Content-Type `application/json; charset=utf-8`. Исключения из конверта: (а) стриминг-тело файла (raw bytes), (б) 202 при инициации download/sendMessage — но их ОШИБКИ тоже в конверте.

### 8.2.4 Пагинация /v1/chats (курсор) + прогрев кэша *(замена numeric offset)*

`[Факт]` `getChats(chat_list, limit)` возвращает только уже загруженные в память чаты, упорядоченные по `(order, chat_id)`, БЕЗ offset. Догрузка — `loadChats(chat_list, limit)`, результат приходит апдейтами `updateNewChat`/`updateChatPosition`, исчерпание — `error 404`. Ответ на сам `loadChats` — это `ok`/`error 404`, а НЕ готовые данные.

`[Решение]` `GET /v1/chats?limit=(default 20,max 100)&cursor=<opaque>`. `ChatCache`:
- После Ready инициировать `loadChats(chatListMain, N)`; подписка на `updateNewChat`/`updateChatPosition`/`updateChatLastMessage`/`updateChatReadInbox/Outbox`; хранить чаты сортированными по `chatPosition.order` desc.
- **Warmup-инвариант:** кэш считается «прогретым» по факту прихода первой порции апдейтов ИЛИ по завершению ограниченной серии `loadChats`. Пока НЕ прогрет и `cursor` пуст: хендлер либо дожидается первой порции (bound по TTL), либо возвращает `503 {state:"warming"}` — но НЕ ложный пустой список с `has_more=false`.
- Курсор = `base64url("{order}:{chat_id}")` последнего отданного. Если в памяти < limit за курсором — `loadChats` (bound: ≤5 итераций / общий таймаут), `error 404` → `has_more=false`. MVP — только `chatListMain`. `[Открытый вопрос]` Archive/папки — `post-MVP`.

### 8.2.5 Пагинация /v1/messages (getChatHistory) *(уточнение цикла)*

`[Факт]` `getChatHistory(chat_id, from_message_id, offset, limit, only_local)` возвращает сообщения СТАРШЕ `from_message_id` (0 = от последнего), newest-first; на ПЕРВЫЙ (некэшированный) вызов часто возвращает 0/мало, пока история не подгружена с сервера; `limit ≤ 100`.

`[Решение]` `from_id` → `from_message_id` (строка, "0" = от последнего). `limit` default 30, max 100. Перед историей гарантировать, что чат загружен (`getChat`/ChatCache). Один вызов `getChatHistory(only_local=false)` уже инициирует сетевую догрузку и возвращает полученное. **Retry-цикл ограничен: 2-3 итерации** с небольшим backoff, общий TTL цикла bound; НЕ держать несколько параллельных витков на один HTTP-запрос (иначе серия таких запросов упрётся в `503 SERVICE_BUSY`). `meta.next_cursor` = message_id самого старого отданного. Документировать: у начала истории выборка КОРОЧЕ limit — это норма, не ошибка.

### 8.2.6 POST /messages: тело, семантика (без Idempotency-Key) *(упрощено)*

`[Факт]` `sendMessage(chat_id, message_thread_id, reply_to:InputMessageReplyTo, options, reply_markup, input_message_content)` с `inputMessageText{formattedText}`. Возвращает message с ВРЕМЕННЫМ id и `messageSendingStatePending` сразу; финал — `updateMessageSendSucceeded`(новый id)/`updateMessageSendFailed`.

`[Решение]` Тело `SendMessageRequest{ text:string(required, непустой), reply_to_message_id?:string, disable_notification?:bool=false, parse_mode?:enum(none|markdown|html)=none }`.
- **Длина текста — в UTF-16 code units** (как td_api), НЕ в UTF-8 code points и НЕ в кодовых точках. Мягкий ранний отсев `> 4096 UTF-16 units`; жёсткую истину оставить за TDLib (его error → `400`). Для эмодзи вне BMP локальная проверка в code points дала бы неверный лимит.
- При `parse_mode!=none` — синхронный `execute(parseTextEntities)`.
- **Idempotency-Key убран из MVP.** Семантика — **at-least-once** (согласовано с 5.13); отдельный код `409 IDEMPOTENCY_KEY_REUSED` и in-memory LRU не нужны. Ключ идемпотентности — `post-MVP`, когда появится клиент, умеющий его слать.

`[Решение]` **Ответ:** `202 Accepted` + `data:MessageDto{sending_state:"pending", temporary_message_id, chat_id}`. Финал — по WS.

`[Решение]` **Межканальный порядок 202↔WS не гарантирован** (разные соединения/loop): `updateMessageSendSucceeded` может уйти в WS РАНЬШЕ, чем клиент получит 202. Поэтому:
- В WS-апдейте `succeeded`/`failed` ВСЕГДА нести И `old_message_id`, И новый `message.id` — апдейт самодостаточен без предшествующего 202.
- 202 содержит достаточную идентификацию (`chat_id` + `temporary_message_id`) до любого WS-события.
- Клиенту рекомендовать буферизацию `succeeded` по `old_message_id`.

### 8.2.7 Не-текстовые сообщения в истории *(новый)*

`[Решение]` `MessageDto` всегда содержит дискриминатор `content.type` (`text`/`photo`/`document`/`sticker`/`unsupported`) и `text.text` только для `messageText`. Не-текстовые — с метаполями + `supported:false`, для медиа даём `remote_unique_id` + `file_type` (см. 8.3′) для download. Сообщения НЕ выкидываются из выборки (пагинация/курсор стабильны). `toJson(MessageContent&)` — `switch` с `default→{type:"unsupported"}`.

### 8.2.8 POST mark-as-read (viewMessages) *(новый, пробел ТЗ 8.2)*

`[Факт]` Актуальная сигнатура: `viewMessages(chat_id, message_ids, source, force_read)`. Ответ — `ok`, состояние сообщений меняется через апдейты (`updateChatReadInbox` и т.п.).

`[Решение]` `POST /v1/chats/{chat_id}/messages/read`, тело `{message_ids:[string], force_read?:bool=true}` → `viewMessages(chat_id, message_ids, source=null, force_read)`. Ответ `200 {ok:true}`. Валидацию принадлежности `message_ids` чату локально НЕ делать (TDLib проверит). Батчинг `message_ids` — как есть. Golden/ошибочный тест обязателен.

### 8.3′ Файлы: модель, download (202 + WS), upload-в-чат *(упрощено)*

`[Факт]` `file.id` (int32) валиден только в рамках текущей сессии процесса TDLib; персистентны `remote.id`/`remote.unique_id` (строки). Прогресс приходит `updateFile` (`request_id==0`). При async download нет явного error-апдейта (файл просто перестаёт быть `is_downloading_active`).

`[Решение]` **FileDto:** `{file_id (ephemeral int32), remote_unique_id (persistent), remote_id, file_type (enum), size, downloaded_size, uploaded_size, progress, local_available:bool}`. Наружу основной адресуемый id — `remote_unique_id`.

`[Решение]` **remote_unique_id → file_type (обязательно).** `getRemoteFile(remote_file_id, file_type)` ТРЕБУЕТ `file_type`, а после рестарта ephemeral `file_id` мёртв. Поэтому:
- В `MessageDto`/`FileDto` (и в персистентном индексе, если нужен) хранить `file_type` рядом с `remote_unique_id`.
- Маршрут: `GET /v1/files/{remote_unique_id}` (ephemeral `file_id` наружу НЕ адресуем). При необходимости эндпоинт принимает `?type=<file_type>` и/или полный `remote_id`. Разрешение: `remote_unique_id (+type) → getRemoteFile → file.id → getFile`.

`[Решение]` **Download — без блокирующего co_await (FileWaitRegistry убран).**
- Если файл уже `is_downloading_completed` и `local.path` непуст — сразу стрим `newFileResponse` (см. ниже).
- Иначе запустить `downloadFile(file_id, priority=1, offset=0, limit=0, synchronous=false)` и СРАЗУ вернуть `202 Accepted` + `data:FileDto{progress}`. Прогресс/готовность — через `updateFile` по WS. Синхронный download внутри HTTP-запроса — `post-MVP`. `[Открытый вопрос → продукт]` HTTP Range/206 — `post-MVP`; MVP — корректный `200` без Range.

`[Решение]` **Отдача готового файла:** `HttpResponse::newFileResponse(local.path, attachmentFileName, contentType)` (sendfile, Range из коробки, без RAM). Content-Type из `mime_type`, имя из `file_name`. Запрещено `newFileResponse` на НЕ докачанном файле (усечённые байты).

`[Решение]` **Upload — только «файл в чат» (preliminaryUploadFile убран).**
- Request-body streaming Drogon (`enableRequestStream()`, чанки в temp-файл `files_directory`, 0700) → `sendMessage(inputMessageDocument{inputFileLocal})`; прогресс — `updateFile` по WS. `[Открытый вопрос → продукт]` multipart vs сырой `application/octet-stream` (второе радикально проще против OOM; рекомендация — octet-stream).
- `preliminaryUploadFile`/`cancelPreliminaryUploadFile` (upload-without-send), `inputFileGenerated`, стриминговый аплоад — `post-MVP`.
- **Компенсация при частичном сбое:** temp-файл удаляется в RAII/finally на ВСЕХ путях (успех/ошибка/таймаут/отмена/дисконнект). Если `sendMessage` отклонён (в т.ч. flood-wait) — temp-файл удалить, клиенту вернуть ошибку «файл не отправлен». Авто-ретрай `sendMessage` запрещён (не-идемпотентно, дубли).
- Temp-каталог 0700, удаление после передачи в TDLib / по TTL / при ошибке. `max_upload_bytes` (default 2 GiB), `max_concurrent_transfers`.

### 8.3a Учёт transfers (RAII-guard) *(новый)*

`[Решение]` Download/upload идут мимо CorrelationMap, поэтому `max_inflight_requests` их НЕ ограничивает — их лимитирует `max_concurrent_transfers`. Каждый активный transfer инкрементит счётчик на входе и ГАРАНТИРОВАННО декрементит через RAII-guard на всех путях (успех/ошибка/таймаут/отмена/дисконнект). Метрика `transfers_active` (диагностическая) покрыть тестом на отсутствие «залипших» счётчиков после дисконнекта в середине передачи (см. 8.10).

### 8.6 Версионирование и совместимость *(новый)*

`[Решение]` `/v1` стабилен, только аддитивные изменения. Клиент — tolerant reader. Ломающие изменения → `/v2`. DTO-слой развязывает контракт от td_api: если поле исчезло — маппер даёт default/null, не 500. Версию TDLib отдавать в заголовке `X-TDLib-Version` (диагностика), не в теле DTO.

### 8.7 Единицы времени *(новый)*

`[Решение]` Все временные поля — unix timestamp в СЕКУНДАХ (как td_api), числом; имена с суффиксом `_at` (`date_`→`date`, `edit_date_`→`edited_at`). Не смешивать с миллисекундами.

### 8.8 Валидация и лимиты тела *(новый)*

`[Решение]` `setClientMaxBodySize` глобально под upload; на JSON-роутах ранняя проверка `req->body().size() > json_max_body` (256 КБ) → `413`. `req->getJsonObject()` null → `400 INVALID_JSON`. Path-параметры парсить явно (`std::from_chars` int64/int32, ошибка → `400`). Общий helper `validateJson(spec)`.

### 8.9 Контроллеры и health/readiness *(новый)*

`[Решение]` Контроллеры `HealthController`, `AuthController`, `MeController`, `ChatController`, `MessageController`, `FileController` (`drogon::HttpController`), методы `Task<HttpResponsePtr>`, пути через `METHOD_ADD` с явными регэкспами числовых сегментов. WS — `UpdatesWebSocketController : drogon::WebSocketController` (на колбэках).
- `GET /v1/health` (liveness, всегда 200 если процесс жив, без auth).
- `GET /v1/ready` (200 только при `authorizationStateReady` И живом свежем receive-потоке И `correlation_pending < max`; иначе 503 с телом `{ready:false, state, authorization_state, connection_state}`; без auth). До готовности receive-потока — `{state:"starting"}` (см. 7.1).

### 8.10 Очистка per-connection состояния *(новый)*

`[Решение]` При `handleConnectionClosed` И при `forceClose` — в ОДНОМ месте: снять соединение из `WsSubscriberRegistry`, обнулить `pending_updates_`, снять его ожидания из активных transfers/стримов, задекрементить `transfers_active` через RAII (см. 8.3a). Тест: дисконнект в середине передачи → счётчики `inflight`/`transfers`/`pending` не «залипают».

---

## 9. Ошибки

### 9.1 Таблица маппинга td_api::error.code → HTTP *(новый)*

`[Решение]`
| td_api code | HTTP | Примечание |
|---|---|---|
| 400 | 400 | VALIDATION/BAD_REQUEST |
| 401 (AUTH_KEY/UNAUTHORIZED) | 401 или 409 | формальный признак — ниже |
| 403 | 403 | |
| 404-подобные (CHAT_NOT_FOUND, MESSAGE_NOT_FOUND) | 404 | |
| 406 | 406 | |
| 420/429 `FLOOD_WAIT_N`/`SLOWMODE_WAIT_N` | 429 | + `Retry-After: N`, `error.retry_after` |
| 500..599 / PHONE_MIGRATE | 502 | |
| **0 / отрицательный / неизвестный (default)** | **502 TELEGRAM_ERROR** | проброс `tdlib_code`/`tdlib_message` |

`[Решение]` **Default-бакет обязателен:** любой непокрытый/нулевой/отрицательный code → `502 TELEGRAM_ERROR` (не 500 без конверта; статический даункаст без default = UB/500).

`[Решение]` **Формальный признак 401→409:** если `AuthStateHolder != Ready` на момент ошибки → `409 AUTH_REQUIRED`, иначе `401`. Без «на словах».

Таймаут моста → 504, сессия не найдена → 404, невалидный ввод → 400. Всегда пробрасывать `tdlib_code`/`tdlib_message` в конверт. Тест полноты таблицы — перебор диапазонов кодов (14.x).

### 9.2 Enum сервисных кодов *(новый)*

`[Решение]` Фиксированный enum `error.code`: `VALIDATION_ERROR`(400), `INVALID_JSON`(400), `UNAUTHENTICATED`(401), `FORBIDDEN`(403), `NOT_FOUND`(404), `AUTH_REQUIRED`(409/428), `FLOOD_WAIT`/`RATE_LIMITED`(429), `SERVICE_STARTING`(503), `SERVICE_BUSY`(503), `SERVICE_CLOSING`(503), `UPSTREAM_TIMEOUT`(504), `TELEGRAM_ERROR`(варьируется). В конверт добавить `error.request_id` (внешний req_id для трассировки). `ErrorMapper::map(const td_api::error&, AuthState) -> {http, ApiError}`.

### 9.3 Flood-wait (реактивно, без token-bucket) *(упрощено)*

`[Решение]` При `error.code==429` (или префикс message `FLOOD_WAIT_`/`SLOWMODE_WAIT_`, код нестабилен) — распарсить секунды регэкспом, вернуть `429 + Retry-After:<sec> + error.retry_after`, метрика `flood_wait_total{method}`, `LOG_WARN{event=flood_wait}` (без контента). **MVP — реактивно сюрфейсить клиенту, авто-ретрай НЕ делать** (для `sendMessage` — тем более, дубли). **Проактивный `FloodWaitManager`/token-bucket и per-chat `retry_after` — убраны из MVP** (§13 сам отключал авто-ретрай, упреждающее throttling дублирует защиту Telegram); `send_rate_*` — `post-MVP`. `[Открытый вопрос → продукт]` авто-ретрай read-методов.

### 9.4 Разграничение req_id + санитизация *(новый)*

`[Решение]` ДВА идентификатора: внешний `req_id` (генерится HTTP-фильтром — UUIDv4 или счётчик; принимать входящий `X-Request-Id`; возвращать в ответе заголовком) и внутренний `td_request_id` (числовой, correlation map). В логах — оба. Один HTTP-запрос (upload → sendMessage) порождает несколько `td_request_id`.

`[Решение]` **Санитизация входящего `X-Request-Id`:** allowlist `[A-Za-z0-9._-]`, длина ≤128; иначе игнорировать и сгенерировать свой. Все внешние строки при записи в JSON-лог проходят через сериализатор (не конкатенацию) — защита от log injection/подделки JSONL. `traceparent` (если принимается, см. 10.8) — строгий парсинг W3C-формата, невалидный → отбросить.

---

## 10. NFR

### 10.1 Liveness vs readiness

См. 8.9. `AuthStateHolder{std::atomic<AuthState>}` обновляется только из потока-приёмника. K8s: `livenessProbe→/v1/health`, `readinessProbe→/v1/ready`, `startupProbe→/v1/health` (большой failureThreshold — долгое открытие БД TDLib).

### 10.2 Метрики (минимальный набор) *(упрощено)*

`[Решение]` Prometheus text exposition, `prometheus-cpp`, отдельный `metrics_port`=9091 (bind внутренняя сеть). Лейбл `method` ограничен именами td_api-функций; **запрещены** chat_id/user_id/контент в лейблах.

**MVP-набор:**
`tgw_auth_state{state}`, `tgw_ready` (0/1), `tgw_bridge_request_duration_seconds{method,outcome}` (histogram, бакеты 0.01…30), `tgw_correlation_pending`, `tgw_inflight_requests`, `tgw_bridge_timeouts_total{method}`, `tgw_receive_iterations_total`, `tgw_flood_wait_total{method}`, `tgw_tdlib_unexpected_close_total`, `tgw_session_revoked_total`.

**По мере появления механизмов** (не блокеры MVP): `tgw_ws_subscribers`, `tgw_ws_pending_updates`, `tgw_forced_ws_disconnects_total`, `tgw_transfers_active`, `tgw_updates_total{update_type}`, `tgw_response_after_ttl_total`, `tgw_receive_stall_total`, `tgw_connection_state{state}`, `tgw_shutdown_drain_duration`. `[Открытый вопрос → продукт]` экспозиция/auth метрик.

### 10.3 Логирование *(новый)*

`[Решение]` Формат — JSON Lines в stdout. Обязательные поля: `ts`(ISO8601 UTC), `level`, `logger`, `event`, `req_id`, `session_id`, `td_method`, `td_request_id`, `td_code`, `latency_ms`, `ws_conn_id`. Уровни: prod=INFO; auth-переходы=INFO; flood-wait/back-pressure/удалённая ревокация=WARN; неожиданный Closed/receive-stall/OpenSSL-линковка=FATAL. **Редакция:** запрещено логировать текст сообщений, номер (маскировать до 2 цифр), код, 2FA-пароль, api_hash, database_encryption_key, Bearer. **Архитектурный запрет:** запрещено передавать `td_api::*` в логгер через `to_string()`; ввести `logging::describe(const td_api::Object&)` (allowlist: имя типа, id-поля, размеры, enum-имена). CI-правило (grep/clang-tidy), банящее `to_string(` и имена секретных полей в аргументах логгера.

### 10.4 Graceful shutdown *(новый)*

`[Решение]` SIGTERM (и SIGINT наравне) → `ShutdownCoordinator::run()` (`std::atomic<bool> shutdown_initiated=true`):
1. readiness NOT_READY (`/v1/ready`→503, LB дренаж).
2. Перестать принимать новые HTTP; новые запросы → 503.
3. `closeWebSocketsGracefully()` — close-frame 1001 по всем WS, дать `ws_drain_seconds`=5.
4. **Дренаж активных transfers/file-стримов** (новый шаг): пройти активные download/upload и file-response, резолвить `SERVICE_SHUTTING_DOWN`(503) через `loop->queueInLoop` ДО остановки loops; прервать активные `downloadFile` (`cancelDownloadFile`); удалить temp-файлы; задекрементить `transfers_active`.
5. Дренаж in-flight CorrelationMap до `shutdown_grace_seconds`=25; каждую claim через `erase`, резолв `SERVICE_SHUTTING_DOWN`(503) через её `loop->queueInLoop` ДО остановки loops; оставшиеся по грейсу → 504.
6. `sendOneWay(close)` и ждать `authorizationStateClosed` до `close_timeout_seconds`=10.
7. `stopping_=true`, join потока-приёмника.
8. `drogon::app().quit()` — строго ПОСЛЕ дренажа (иначе `queueInLoop` на остановленный loop → resume потерян, кадр корутины утечёт).

`[Решение]` **Повторный сигнал:** второй SIGTERM/SIGINT (флаг `already_shutting_down`) → немедленный `_exit(1)` с логом `force_shutdown`.

`[Решение]` **Бюджет дренажа < terminationGracePeriodSeconds.** Сумма `ws_drain 5 + grace 25 + close 10 = 40с` ДОЛЖНА быть меньше k8s `terminationGracePeriodSeconds`. В деплой-манифесте выставить `terminationGracePeriodSeconds=45` (в v2.1 был противоречивый 30 < 40 — исправлено).

### 10.5 Watchdog receive-потока *(новый)*

`[Решение]` Поток-приёмник инкрементит `receive_iterations` и пишет `last_iteration_monotonic` каждую итерацию. Watchdog (таймер Drogon, период 5с; НЕ активен до первого успешного receive-итератора): если `now - last_iteration > receive_watchdog_seconds`=10 → `LOG_FATAL` + flush, `tgw_receive_stall_total++`, `_exit(1)` (см. 7.4a, не `std::exit`). `/v1/ready` учитывает свежесть `last_iteration`.

### 10.6 Восстановление после рестарта/сети *(новый)*

`[Решение]` Матрица:
- **Переживает** (volume): ключи авторизации, кэш чатов TDLib-БД, скачанные файлы.
- **НЕ переживает** (in-memory): correlation map (незавершённые HTTP теряются → клиент повторяет), WS-подписки (клиент переподключается), fan-out (апдейты теряются, at-most-once, gap-fill через `getChatHistory`).
- Возврат в Ready после рестарта обеспечивает `StartupBootstrapper` (7.1), НЕ HTTP-логин.
- Потеря сети: TDLib реконнектит сам; экспорт `updateConnectionState` как `tgw_connection_state`. `[Открытый вопрос]` При `connectionStateWaitingForNetwork` дольше `connection_degraded_seconds`=30 — политика readiness. При не-Ready продлевать TTL / отдавать 503 (connecting) вместо ложного 504.

### 10.7 Конфигурация *(дополнение)*

`[Решение]` Ключи (с учётом упрощений): `shutdown_grace_seconds`=25, `ws_drain_seconds`=5, `close_timeout_seconds`=10, `receive_watchdog_seconds`=10, `td_receive_timeout_seconds`=1.0, `auth_transition_timeout_seconds`, `ws_max_pending_updates`=1000, `max_inflight_requests`=2000, `correlation_ttl_ms`=30000, `correlation_sweep_ms`=1000, `connection_degraded_seconds`=30, `max_upload_bytes`=2GiB, `max_concurrent_transfers`, `max_ws_connections`, `metrics_port`=9091, `listen_address`=127.0.0.1, `listen_port`, `behind_tls_proxy`/`allow_insecure_bind`, `trusted_proxies`, `json_max_body`=256KiB, `log_level`=info, `log_format`=json, `tdlib_log_verbosity`=1, `strict_perms`=true.

Убраны из MVP (`post-MVP`): `max_update_queue`, `ws_send_buffer_high_watermark`, `max_ws_connections_per_token`, `send_rate_per_chat`, `send_rate_global`.

### 10.8 Трассировка *(упрощено)*

`[Решение]` **MVP — только `req_id`** (генерация + `X-Request-Id` passthrough с санитизацией 9.4 + заголовок в ответе). W3C `traceparent`/`trace_id` и OpenTelemetry — этап 6 (не блокер).

### 10.9 Алерты

`[Открытый вопрос → этап 6]` Prometheus alert-rules (auth!=Ready >2мин, receive-stall, flood-wait rate, correlation_pending near limit, connection WaitingForNetwork >5мин, p99 latency).

---

## 11. Сборка и деплой

### 11.1 Линковка TDLib *(правка решения #4)*

`[Факт]` Экспортируемый таргет `Td::TdShared`/`Td::TdJson` (`libtdjson.so`) отдаёт ТОЛЬКО C JSON-интерфейс. `td::ClientManager` и типы `td_api::*` доступны ИСКЛЮЧИТЕЛЬНО через СТАТИЧЕСКИЙ `Td::TdStatic`.
`[Решение]` `target_link_libraries(app PRIVATE Td::TdStatic Drogon::Drogon)`. TDLib встраивается статически (транзитивно OpenSSL/ZLib/libstdc++). В решении #4 и разделе 11 убрать «динамическую линковку TDLib» и `Td::TdShared`. .so самой TDLib в образ НЕ копируется.

### 11.2 OpenSSL

`[Решение]` Единый OpenSSL для TDLib, Drogon/Trantor и нашего таргета. `find_package(OpenSSL 3.0 REQUIRED)` + `FATAL_ERROR` при `< 3.0`. Запретить vendored/BoringSSL/LibreSSL. Мажор 3.0.x (Debian bookworm).

### 11.3 Тулчейн и стандарт

`[Решение]` TDLib, Drogon и наш таргет — ОДИН компилятор и одна версия libstdc++ (единый builder-образ, GCC 12 bookworm). TDLib с `-DCMAKE_CXX_STANDARD=17`, наш таргет — C++20 (C++17↔C++20 ABI-совместимы в одном мажоре GCC). Запретить микс GCC+Clang. Пин компилятора в CMakePresets.

### 11.4 Целевая архитектура *(упрощено)*

`[Решение]` ~~MVP — одна целевая архитектура~~ **Реализовано multi-arch с самого начала:** CI собирает amd64 + arm64 (`image:<sha>-<arch>` + манифест `image:<sha>` через `buildx imagetools`) — парк раннеров смешанный, а дев-машина arm64. Кэш-образ builder (11.6) есть для обеих архитектур. Для TDLib: `-DTD_ENABLE_LTO=OFF -DTD_ENABLE_JNI=OFF -DCMAKE_BUILD_TYPE=Release`, `-j2` при малом RAM (≥8 ГБ на TDLib-стадию).

### 11.5 glibc builder↔runtime

`[Решение]` `[Открытый вопрос → комплаенс]` пара builder `debian:12-slim` (glibc 2.36) → runtime `gcr.io/distroless/cc-debian12`, обе по digest `@sha256`. В финал копировать по `ldd $BIN`: `libssl.so.3`, `libcrypto.so.3`, `libz.so.1`. Одинаковый glibc-мажор builder и runtime.

### 11.6 Кэш TDLib

`[Решение]` Отдельный `Dockerfile.tdlib` собирает TDLib на пине `TDLIB_REF`, пушит как `tdlib-base:${TDLIB_REF}` (для целевого arch). Пересборка только при изменении файла-пина (`rules: changes:[TDLIB_REF]`). Основной Dockerfile: `ARG TDLIB_REF` + `FROM tdlib-base:${TDLIB_REF} AS tdlib`. Доп: BuildKit `--mount=type=cache` (ccache) + registry cache.

### 11.7 Пиннинг версий и рантайм-проверка

`[Решение]` TDLib — полный git-SHA в `TDLIB_REF` (файл в репо, читается CMake/Docker). На старте `getOption("version")`, сравнить с `kExpectedTdlibVersion`, при расхождении — FATAL. Drogon — пин `DROGON_REF` (≥1.8.x для стабильного `Task<HttpResponsePtr>`). Обновление TDLib — отдельная `feature/*` с прогоном тестов моста.

### 11.8 Healthcheck в distroless

`[Решение]` distroless без shell/curl → под-команда бинаря `app --healthcheck` (локальный GET `/v1/health`, exit 0/1). `HEALTHCHECK --interval=15s --timeout=3s --start-period=20s CMD ["/app","--healthcheck"]`.

### 11.9 Non-root и volume

`[Решение]` `USER 65532:65532` (distroless nonroot). В builder: `RUN install -d -m 0700 -o 65532 -g 65532 /data/session /data/files`. `VOLUME ["/data/session","/data/files"]`. Конфиг `database_directory=/data/session`, `files_directory=/data/files`. При bind-mount — хост-каталог `chown 65532` заранее.

### 11.10 Секреты и Drogon-статик

`[Решение]` Конвенция `*_FILE` (`DATABASE_ENCRYPTION_KEY_FILE`, `API_HASH_FILE`, `BEARER_TOKENS_FILE`) — чтение из `/run/secrets/*`, приоритет над прямым env. Не использовать `ENV`/build-arg для секретов. Drogon статически (`BUILD_SHARED_LIBS=OFF`, jsoncpp/trantor внутрь), отключить необязательное (`BUILD_BROTLI=OFF`, `BUILD_YAML_CONFIG=OFF`, `BUILD_ORM=OFF`). Финальный список .so — из `ldd`, скриптом.

### 11.11 CMake presets и hardening

`[Решение]` `CMakePresets.json` v6: `dev-debug`, `ci-release` (Ninja, `EXPORT_COMPILE_COMMANDS=ON`), hidden base с пинами компилятора/`CMAKE_CXX_STANDARD=20`/prefix path, `find_package(Td CONFIG REQUIRED)`+`find_package(Drogon CONFIG REQUIRED)`, `-Wall -Wextra -Wpedantic` на НАШ таргет. Изолировать `#include td_api.h` в 1-2 TU/PCH. Рекомендованный запуск: `--read-only`, `--tmpfs /tmp`, `--cap-drop=ALL`, `--security-opt no-new-privileges`, `.dockerignore`, опц. trivy-скан.

---

## 12. Этапы

`[Решение]`
- **Этап 0** (`feature/skeleton`): `.clang-format` (llvm/google базис), `.clang-tidy` (`bugprone-*`, `concurrency-*`, `cppcoreguidelines-*`, `performance-*`, `misc-*`, WarningsAsErrors на `bugprone-`/`concurrency-`), `td.supp`/`lsan.supp` только для сторонних символов (наш код в suppressions запрещён, проверять грепом). Ввести `ITdTransport`/`FakeTdTransport` и инъекцию времени уже здесь.
- **Этап 1** (`feature/td-bridge`): property/stress-тест корреляции (10k параллельных invoke, эхо по request_id, нет cross-talk/double-resume) под TSan.
- **Этап 2** (`feature/auth`): `StartupBootstrapper` + `AuthStateManager` + auth-mutex; интеграционный контур на **test DC** (`use_test_dc=true`, номер `99966X_YYYY`, код = цифра DC повторённая). Фикстура предавторизованной сессии (bootstrap-джоб доходит до Ready, публикует зашифрованный session-каталог артефактом/кэшом). `[Открытый вопрос → инфра]` тестовый аккаунт/ToS.
- **Этап 6**: sanitizers, нагрузка через FakeTdTransport-генератор апдейтов; OpenTelemetry/traceparent; alert-rules.

---

## 13. Безопасность

`[Решение]`
- **database_encryption_key:** 32 байта CSPRNG, base64/hex в ВНЕШНЕМ секрете. Читать один раз на старте; **авто-генерация на старте запрещена** (иначе полная потеря сессии). Утеря = полный ре-логин (в критериях приёмки). **Ротация ключа (`database_encryption_key_new` → `setDatabaseEncryptionKey`) и hot-rotation — убраны из MVP** (`post-MVP`).
- **Core dump / память:** на старте `prctl(PR_SET_DUMPABLE,0)`, `setrlimit(RLIMIT_CORE,{0,0})`. Ключ/api_hash после `setTdlibParameters` затирать `OPENSSL_cleanse`. Дамп конфига маскирует секреты `***`.
- **Лог TDLib:** первыми вызовами после `create_client_id()` (в `StartupBootstrapper`) — `setLogVerbosityLevel{1}` и `setLogStream{logStreamEmpty}` (или `logStreamFile` 0600 с ротацией). `tdlib_log_verbosity` default 1, прод max 2.
- **DoS:** `max_upload_bytes`, `max_concurrent_transfers`, `max_ws_connections`, `max_inflight_requests`. Temp-каталог 0700, фоновая очистка осиротевших temp-файлов.
- **Права файлов:** на старте `umask(077)` (файлы TDLib → 0600). Проверка прав `database_directory`: group/other-биты → chmod 0700 + warning или отказ (`strict_perms`=true).
- **Flood-wait:** реактивно (§9.3), без проактивного token-bucket. Не ретраить молча.
- **Бэкап:** ключ шифрования бэкапить ОТДЕЛЬНО от каталога (secret manager). Консистентный снимок — `close` перед бэкапом или volume snapshot.
- **Bearer:** статичные токены из секрета, SHA-256 предвычисление + timing-safe сравнение. Ротация — через рестарт. SIGHUP hot-reload и структурированный формат токенов — `post-MVP`.

---

## 14. Критерии приёмки *(дополнения)*

`[Решение]`
- **14.2:** цикл авторизации проверяет `/v1/ready` (200 только при Ready) отдельно от `/v1/health`.
- **14.9** (расширить): на MR `feature→develop` — `clang-format --dry-run --Werror`, `clang-tidy` (гейт), unit под ASan+UBSan+LSan, unit под TSan (таргеты bridge/correlation/update_router против FakeTdTransport), coverage ≥85% строк по этим таргетам. На `release→main`/ночной — интеграция на test DC + нагрузка (SLO).
- **14.10:** разрыв HTTP-клиента до ответа → запись резолвится безопасно, поздний Response отбрасывается без UAF (ASan); гонка `sweepExpired`+`pushResponse` — ровно одна сторона резолвит (CAS фазы).
- **14.11:** back-pressure — медленный WS-клиент (не читает сокет) при заливке выше порога закрывается (1013 опц.), RSS не растёт неограниченно, остальные подписчики получают апдейты в порядке.
- **14.12:** `updateAuthorizationState=Closed` при N in-flight (invoke + активные download) → все получают 503 SERVICE_CLOSING/SHUTTING_DOWN, temp-файлы удалены, receive-поток join'ится (нет leak под LSan).
- **14.13:** flood-wait — `pushResponse(error{420,"FLOOD_WAIT_30"})` → конверт `{ok:false,error:{code:"FLOOD_WAIT",tdlib_code:420,retry_after:30}}`, запись удалена, мост жив.
- **14.14:** golden-тесты DTO-проекции (JSON-снапшоты) + allowlist-тест (скрытые поля наружу не попадают).
- **14.15:** реальные ответы проходят валидацию по вручную поддерживаемому `docs/openapi.yaml` (единый источник истины HTTP-контракта).
- **14.5** (уточнить): пиковый RSS при K параллельных download >50МБ не превышает потолка SLO (стрим не буферизует файл целиком); тест отмены в середине стрима (ASan). `[Открытый вопрос → продукт]` числовые SLO.
- **14.16** (новый): рестарт с volume → сервис сам доходит до Ready через `StartupBootstrapper` без вызова `/v1/auth/*`.
- **14.17** (новый): 2 параллельных `/code` сериализуются auth-mutex, второй получает детерминированную ошибку, состояние консистентно.
- **14.18** (новый): shutdown при активных download → все 503, temp-файлы удалены, `transfers_active`→0, нет leak (LSan).
- **14.19** (новый): `POST /logout` → `LoggingOut→Closed` без `_exit(1)`; последующий `POST /auth/session` стартует новый логин.
- **14.20** (новый): подмена `database_encryption_key` при существующей БД → детерминированный отказ старта (`DB_KEY_MISMATCH`), БД не повреждена.
- **14.21** (новый): маппинг ошибок (`httpStatusForTdError`) — code 400 без "not found" в сообщении → 400, code 400 с "not found" → 404, code 429 / `FLOOD_WAIT` / "Too Many Requests" → 429 (+`Retry-After`, если распарсился), всё остальное (default-бакет, включая синтетические `SERVICE_BUSY`/`UPSTREAM_TIMEOUT` моста) → 502.
- **14.22** (новый): 202 на `sendMessage` + `updateMessageSendSucceeded` по WS самодостаточен (несёт `old_message_id`+`message.id`) при любом порядке доставки.
- **14.23** (новый): `X-Request-Id` с CR/LF/управляющими символами игнорируется, лог-строки JSONL не подделываются.
- **14.24** (новый): download по `remote_unique_id`(+`file_type`) работает после рестарта (ephemeral `file_id` мёртв).

---

## Открытые вопросы к заказчику

1. **Семантика ответа `sendMessage`** — принятая рекомендация 202 + финал по WS устраивает? Idempotency-Key действительно откладываем до появления умеющего его клиента?
2. **`wait_registration` (незарегистрированный номер)** — MVP отвечает `409 AUTH_REQUIRED`. Регистрация нового аккаунта в скоупе вообще?
3. **Удалённая ревокация сессии (`LoggingOut` не по нашей инициативе)** — degraded-режим (рекомендация) или процесс должен завершаться для рестарта?
4. **Download** — MVP отдаёт `202` + прогресс по WS (без блокирующего ожидания). Синхронное ожидание докачки и HTTP Range/206 нужны в MVP или можно `post-MVP`?
5. **Upload** — сырой `application/octet-stream` (рекомендация, проще против OOM) или обязателен `multipart/form-data`?
6. **Числовые SLO** (p99 latency, пиковый RSS, число параллельных download) — какие целевые значения?
7. **Метрики** — экспозиция `metrics_port` наружу и нужна ли на нём авторизация?
8. **Целевая архитектура** — только amd64 для MVP, или arm64-деплой требуется сразу (влияет на CI-матрицу)?
9. **connection `WaitingForNetwork` > 30с** — держать readiness/буферизовать до TTL или сразу `503`?
10. **Тестовый Telegram-аккаунт на test DC** и соответствие ToS — кто предоставляет?
11. **Комплаенс на базовые образы** (`debian:12-slim` / `distroless/cc-debian12` по digest) — согласовано?

---

## Что закрыть до старта кодирования (blockers)

1. **Секреты для старта:** `api_id`, `api_hash`, `database_encryption_key` (32 байта, стабильный, внешний), список Bearer-токенов — предоставлены в формате `*_FILE`/секрет-менеджера. Без них не поднимется даже `StartupBootstrapper`.
2. **Пин TDLib (`TDLIB_REF`, полный git-SHA) и `kExpectedTdlibVersion`** зафиксированы; согласована сигнатура `setTdlibParameters`/`viewMessages`/`getChatHistory` под выбранным SHA (проверить по факту, а не по памяти).
3. **Пин Drogon (`DROGON_REF` ≥1.8.x)** с рабочим `Task<HttpResponsePtr>` — проверен на сборку статикой с той же OpenSSL 3.0.x.
4. **Единый тулчейн** (GCC 12 bookworm, единый OpenSSL 3.0.x, статическая TDLib через `Td::TdStatic`) собран в builder-образе; `Dockerfile.tdlib` + `tdlib-base` кэш работают.
5. **`ITdTransport`/`FakeTdTransport` + инъекция времени (`SteadyClock`/FakeClock)** введены как фундамент (этап 0) — от них зависят все TSan/unit-тесты моста.
6. **Инвариант старта подсистем** зафиксирован в коде: receive-поток → `StartupBootstrapper` (до первого `updateAuthorizationState`) → drogon listen; watchdog не активен до первого receive-итератора.
7. **`terminationGracePeriodSeconds` в деплой-манифесте выставлен `= 45`** (> суммы бюджета дренажа 40с) — иначе SIGKILL в момент записи БД.
8. **`docs/openapi.yaml`** заведён как единый источник истины HTTP-контракта (id — строки, конверт, коды ошибок) — под него пишутся golden/валидационные тесты (14.15).
9. **CI-гейты** (`clang-format`, `clang-tidy` WarningsAsErrors на `bugprone-`/`concurrency-`, ASan/UBSan/LSan, TSan на bridge/correlation/update_router, coverage ≥85%) настроены до первого функционального PR.
10. **Volume-каталоги** `/data/session` и `/data/files` (0700, owner 65532) и политика бэкапа ключа ОТДЕЛЬНО от каталога — согласованы с инфрой.