#pragma once

#include "bridge/transport.hpp"

#include <td/telegram/td_api.h>

#include <deque>
#include <mutex>

namespace tgw::testing {

// Потокобезопасный scripted-транспорт для unit/TSan-тестов моста без реального TDLib.
// pushResponse(rid, obj) — ответ на конкретный request_id; pushUpdate(obj) — апдейт (rid==0).
class FakeTdTransport final : public tgw::bridge::ITdTransport {
   public:
    std::int32_t createClientId() override {
        std::lock_guard lock(mutex_);
        return ++last_client_id_;
    }

    void send(std::int32_t client_id, std::uint64_t request_id,
              td::td_api::object_ptr<td::td_api::Function> fn) override {
        std::lock_guard lock(mutex_);
        sent_.push_back(SentItem{client_id, request_id, std::move(fn)});
    }

    tgw::bridge::Response receive(double /*timeout_seconds*/) override {
        std::lock_guard lock(mutex_);
        if (inbox_.empty()) {
            return tgw::bridge::Response{};  // object == nullptr => штатный таймаут
        }
        auto resp = std::move(inbox_.front());
        inbox_.pop_front();
        return resp;
    }

    void pushResponse(std::int32_t client_id, std::uint64_t request_id,
                      td::td_api::object_ptr<td::td_api::Object> obj) {
        std::lock_guard lock(mutex_);
        inbox_.push_back(tgw::bridge::Response{client_id, request_id, std::move(obj)});
    }

    void pushUpdate(std::int32_t client_id, td::td_api::object_ptr<td::td_api::Object> obj) {
        std::lock_guard lock(mutex_);
        inbox_.push_back(tgw::bridge::Response{client_id, 0, std::move(obj)});
    }

    std::size_t sentCount() {
        std::lock_guard lock(mutex_);
        return sent_.size();
    }

   private:
    struct SentItem {
        std::int32_t client_id;
        std::uint64_t request_id;
        td::td_api::object_ptr<td::td_api::Function> fn;
    };

    std::mutex mutex_;
    std::deque<tgw::bridge::Response> inbox_;
    std::deque<SentItem> sent_;
    std::int32_t last_client_id_ = 0;
};

}  // namespace tgw::testing
