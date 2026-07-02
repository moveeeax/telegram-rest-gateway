#include "bridge/td_bridge.hpp"

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <utility>

namespace tgw::bridge {

// ---------------- TdAwaitable ----------------

TdAwaitable::TdAwaitable(TdBridge& bridge, std::int32_t client_id,
                         td::td_api::object_ptr<td::td_api::Function> fn)
    : bridge_(bridge), client_id_(client_id), fn_(std::move(fn)) {}

bool TdAwaitable::await_suspend(std::coroutine_handle<> handle) {
    state_ = std::make_shared<RequestState>();
    state_->request_id = bridge_.id_gen_.next();
    state_->handle = handle;
    // Резолв вернётся в ЭТОТ event-loop (§5.6). В HTTP-контексте это IO-loop Drogon.
    state_->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if (state_->loop == nullptr) {
        state_->loop = drogon::app().getLoop();
    }
    state_->deadline = bridge_.clock_() + bridge_.config_.ttl;

    // insert-before-send строго в этом порядке (§5.7).
    if (!bridge_.registerAndSend(state_, std::move(fn_), client_id_)) {
        // Превышен лимит in-flight: не подвешиваемся, резолвим сразу (§5.11).
        state_->phase = Phase::Rejected;
        state_->result = makeError(503, "SERVICE_BUSY");
        return false;  // await_resume вызовется немедленно
    }
    return true;  // подвесились; разбудит поток-приёмник или TTL-sweeper
}

td::td_api::object_ptr<td::td_api::Object> TdAwaitable::await_resume() {
    return std::move(state_->result);
}

// ---------------- TdBridge ----------------

TdBridge::TdBridge(ITdTransport& transport, IUpdateSink& sink, BridgeConfig config,
                   SteadyClock clock)
    : transport_(transport),
      sink_(sink),
      config_(config),
      clock_(std::move(clock)),
      corr_(config.max_inflight) {}

TdBridge::~TdBridge() {
    stop();
}

std::int32_t TdBridge::createClientId() {
    return transport_.createClientId();
}

TdAwaitable TdBridge::invoke(std::int32_t client_id,
                             td::td_api::object_ptr<td::td_api::Function> fn) {
    return TdAwaitable(*this, client_id, std::move(fn));
}

void TdBridge::sendOneWay(std::int32_t client_id, td::td_api::object_ptr<td::td_api::Function> fn) {
    transport_.send(client_id, id_gen_.next(), std::move(fn));
}

bool TdBridge::registerAndSend(const RequestStatePtr& state,
                               td::td_api::object_ptr<td::td_api::Function> fn,
                               std::int32_t client_id) {
    if (!corr_.tryInsert(state)) {
        return false;
    }
    transport_.send(client_id, state->request_id, std::move(fn));
    return true;
}

void TdBridge::resolve(const RequestStatePtr& state, Phase phase,
                       td::td_api::object_ptr<td::td_api::Object> result) {
    {
        std::lock_guard<std::mutex> lock(state->m);
        if (state->phase != Phase::Pending) {
            return;  // уже резолвлено другой стороной — двойная защита (§5.6)
        }
        state->phase = phase;
        state->result = std::move(result);
    }
    trantor::EventLoop* loop = state->loop;
    // resume строго через исходный loop; RequestStatePtr захвачен по значению => жив до resume.
    loop->queueInLoop([state]() {
        if (state->handle) {
            state->handle.resume();
        }
    });
}

void TdBridge::dispatch(Response&& resp) {
    if (resp.request_id != 0) {
        RequestStatePtr state = corr_.claim(resp.request_id);
        if (!state) {
            // Ответ после TTL или неизвестный request_id — отбрасываем (§5.8).
            return;
        }
        resolve(state, Phase::Fulfilled, std::move(resp.object));
    } else {
        sink_.onUpdate(std::move(resp.object));
    }
}

void TdBridge::sweepExpired(std::chrono::steady_clock::time_point now) {
    for (const RequestStatePtr& state : corr_.claimExpired(now)) {
        resolve(state, Phase::TimedOut, makeError(504, "UPSTREAM_TIMEOUT"));
    }
}

void TdBridge::runReceiveLoop() {
    last_sweep_ = clock_();
    while (!stopping_.load(std::memory_order_relaxed)) {
        receive_iterations_.fetch_add(1, std::memory_order_relaxed);
        Response resp = transport_.receive(config_.receive_timeout_seconds);

        const auto now = clock_();
        if (now - last_sweep_ >= config_.sweep_interval) {
            sweepExpired(now);
            last_sweep_ = now;
        }

        if (resp.object == nullptr) {
            continue;  // штатный таймаут receive()
        }
        try {
            dispatch(std::move(resp));
        } catch (const std::exception& e) {
            LOG_ERROR << "dispatch failed: " << e.what();
        } catch (...) {
            LOG_ERROR << "dispatch failed: unknown exception";
        }
    }
}

void TdBridge::start() {
    stopping_.store(false, std::memory_order_relaxed);
    receiver_ = std::thread([this]() { runReceiveLoop(); });
}

void TdBridge::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (receiver_.joinable()) {
        receiver_.join();
    }
}

}  // namespace tgw::bridge
