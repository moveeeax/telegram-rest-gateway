#include "bridge/message_send_tracker.hpp"

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <utility>

namespace tgw::bridge {

// Внутреннее состояние одного ожидания: им одновременно владеют запись в map_, awaiter и (до
// срабатывания) таймер-колбэк. Единоличный резолв обеспечивает атомарный find+erase из map_
// (claim) — как в CorrelationMap: кто первым извлёк узел, тот владеет резолвом. mutex m + resolved
// — defence-in-depth по образцу RequestState/TdBridge::resolve: страхуют result/resolved на случай,
// если обе стороны всё же коснутся состояния.
struct SendWaitState {
    std::mutex m;
    bool resolved = false;
    std::coroutine_handle<> handle;
    trantor::EventLoop* loop = nullptr;
    std::optional<MessageSendOutcome> result;  // nullopt после claim() timeout-веткой
};

// ---------------- MessageSendTracker::Awaitable ----------------

MessageSendTracker::Awaitable::Awaitable(MessageSendTracker& tracker, std::int64_t old_message_id,
                                         std::chrono::milliseconds timeout)
    : tracker_(tracker), old_message_id_(old_message_id), timeout_(timeout) {}

bool MessageSendTracker::Awaitable::await_suspend(std::coroutine_handle<> handle) {
    state_ = std::make_shared<SendWaitState>();
    state_->handle = handle;
    // Резолв (или таймаут) вернётся в ЭТУ петлю (как в TdAwaitable). В HTTP-контексте — IO-loop.
    state_->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (state_->loop == nullptr) {
        state_->loop = drogon::app().getLoop();
    }

    // Регистрируем ожидание ДО планирования таймера (порядок как insert-before-send в мосте).
    if (!tracker_.tryInsert(old_message_id_, state_)) {
        // Уже есть ожидание с таким old_message_id (в норме id уникальны) — не подвешиваемся,
        // await_resume вернёт nullopt.
        return false;
    }

    // Планируем таймаут на исходной петле. Колбэк исполнится на потоке ЭТОЙ петли — на том же,
    // где крутится корутина, поэтому резюмируем хендл напрямую, без queueInLoop. Захватываем
    // ТОЛЬКО долгоживущее (tracker, id, shared state), а НЕ this: к моменту срабатывания таймера
    // awaiter-временный уже может быть разрушен (если раньше сработал resolve*() и корутина
    // завершилась), тогда как tracker и state его переживут. Таймер не отменяется явно — при
    // проигрыше гонки он безвреден (claim вернёт nullptr => no-op).
    MessageSendTracker* tracker = &tracker_;
    const std::int64_t id = old_message_id_;
    std::shared_ptr<SendWaitState> state = state_;
    state_->loop->runAfter(std::chrono::duration<double>(timeout_).count(),
                           [tracker, id, state]() {
                               // Атомарный find+erase: если запись ещё в map_ — таймаут выиграл
                               // гонку у resolve*().
                               std::shared_ptr<SendWaitState> claimed = tracker->claim(id);
                               if (!claimed) {
                                   return;  // resolve*() уже забрал узел — no-op.
                               }
                               std::coroutine_handle<> resume_handle;
                               {
                                   std::lock_guard<std::mutex> lock(claimed->m);
                                   if (claimed->resolved) {
                                       return;
                                   }
                                   claimed->resolved = true;
                                   // result остаётся nullopt — сигнал «таймаут без резолва».
                                   resume_handle = claimed->handle;
                               }
                               if (resume_handle) {
                                   resume_handle.resume();
                               }
                           });
    return true;  // подвесились; разбудит поток-приёмник (resolve*) или таймер
}

std::optional<MessageSendOutcome> MessageSendTracker::Awaitable::await_resume() {
    return std::move(state_->result);
}

// ---------------- MessageSendTracker ----------------

bool MessageSendTracker::tryInsert(std::int64_t old_message_id,
                                   std::shared_ptr<SendWaitState> state) {
    std::lock_guard<std::mutex> lock(mutex_);
    // emplace не перезапишет существующий узел: .second == false при коллизии id.
    return map_.emplace(old_message_id, std::move(state)).second;
}

std::shared_ptr<SendWaitState> MessageSendTracker::claim(std::int64_t old_message_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(old_message_id);
    if (it == map_.end()) {
        return nullptr;
    }
    std::shared_ptr<SendWaitState> state = std::move(it->second);
    map_.erase(it);
    return state;
}

void MessageSendTracker::resolveWith(std::int64_t old_message_id, MessageSendOutcome outcome) {
    // Тот же атомарный find+erase, что и в timeout-ветке: узел заберёт лишь одна сторона.
    std::shared_ptr<SendWaitState> state = claim(old_message_id);
    if (!state) {
        // Никто не ждёт этот id, либо таймаут уже забрал узел — no-op.
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->m);
        if (state->resolved) {
            return;
        }
        state->resolved = true;
        state->result = std::move(outcome);
    }
    trantor::EventLoop* loop = state->loop;
    // Штатно resume маршалится в исходную петлю (как TdBridge::resolve): queueInLoop гарантирует
    // resume на нужном потоке. Если петля уже погашена (shutdown) — синхронный best-effort, чтобы
    // кадр корутины освободился и хендлер не завис.
    if (loop == nullptr || !loop->isRunning()) {
        LOG_WARN << "MessageSendTracker resolve: target event loop not running, resuming "
                    "synchronously (old_message_id="
                 << old_message_id << ")";
        if (state->handle) {
            state->handle.resume();
        }
        return;
    }
    // state захвачен по значению => жив до resume.
    loop->queueInLoop([state]() {
        if (state->handle) {
            state->handle.resume();
        }
    });
}

void MessageSendTracker::resolveSucceeded(std::int64_t old_message_id, Json::Value message_json) {
    MessageSendOutcome outcome;
    outcome.succeeded = true;
    outcome.message = std::move(message_json);
    resolveWith(old_message_id, std::move(outcome));
}

void MessageSendTracker::resolveFailed(std::int64_t old_message_id, std::int32_t error_code,
                                       std::string error_message) {
    MessageSendOutcome outcome;
    outcome.succeeded = false;
    outcome.error_code = error_code;
    outcome.error_message = std::move(error_message);
    resolveWith(old_message_id, std::move(outcome));
}

}  // namespace tgw::bridge
