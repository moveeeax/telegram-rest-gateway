#pragma once

#include <td/telegram/td_api.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "bridge/clock.hpp"
#include "bridge/correlation_map.hpp"
#include "bridge/request_id.hpp"
#include "bridge/request_state.hpp"
#include "bridge/transport.hpp"
#include "bridge/update_sink.hpp"

namespace tgw::bridge {

struct BridgeConfig {
    std::size_t max_inflight = 2000;
    std::chrono::milliseconds ttl{30000};
    std::chrono::milliseconds sweep_interval{1000};
    double receive_timeout_seconds = 1.0;
};

class TdBridge;

// Awaitable моста (SPEC_ELABORATION §5.7): insert-before-send строго внутри await_suspend,
// resume — только через loop->queueInLoop исходного event-loop.
class TdAwaitable {
  public:
    TdAwaitable(TdBridge& bridge, std::int32_t client_id,
                td::td_api::object_ptr<td::td_api::Function> fn);

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    td::td_api::object_ptr<td::td_api::Object> await_resume();

  private:
    TdBridge& bridge_;
    std::int32_t client_id_;
    td::td_api::object_ptr<td::td_api::Function> fn_;
    RequestStatePtr state_;
};

// Мост async TDLib <-> request/response. Владеет транспортом, таблицей корреляции,
// генератором id и единственным потоком-приёмником (§5.3, §5.8).
class TdBridge {
  public:
    TdBridge(ITdTransport& transport, IUpdateSink& sink, BridgeConfig config,
             SteadyClock clock = systemSteadyClock());
    TdBridge(const TdBridge&) = delete;
    TdBridge& operator=(const TdBridge&) = delete;
    ~TdBridge();

    std::int32_t createClientId();

    void start();  // запускает поток-приёмник
    void stop();   // останавливает и join'ит

    // co_await invoke(...) -> object_ptr<Object> (типизированный ответ или error).
    TdAwaitable invoke(std::int32_t client_id, td::td_api::object_ptr<td::td_api::Function> fn);

    // Явный fire-and-forget (напр. close при shutdown): ответ, если придёт, отбрасывается.
    void sendOneWay(std::int32_t client_id, td::td_api::object_ptr<td::td_api::Function> fn);

    // Метрики (диагностика/тесты).
    std::uint64_t receiveIterations() const { return receive_iterations_.load(); }
    std::size_t pending() { return corr_.size(); }

  private:
    friend class TdAwaitable;

    bool registerAndSend(const RequestStatePtr& state,
                         td::td_api::object_ptr<td::td_api::Function> fn, std::int32_t client_id);
    void runReceiveLoop();
    void dispatch(Response&& resp);
    void sweepExpired(std::chrono::steady_clock::time_point now);
    static void resolve(const RequestStatePtr& state, Phase phase,
                        td::td_api::object_ptr<td::td_api::Object> result);

    ITdTransport& transport_;
    IUpdateSink& sink_;
    BridgeConfig config_;
    SteadyClock clock_;
    CorrelationMap corr_;
    RequestIdGenerator id_gen_;
    std::thread receiver_;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> receive_iterations_{0};
    std::chrono::steady_clock::time_point last_sweep_{};
};

}  // namespace tgw::bridge
