#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace tgw::http {

// Idempotency-Key (decision C5): LRU-кэш «ключ -> завершённый ответ» для безопасных ретраев
// отправки. Ключ занятый, но без ответа (запрос в полёте) -> claim() возвращает 1 (409 наружу).
// In-memory: переживает ретраи клиента, не рестарт процесса (документировано).
//
// ИНВАРИАНТ order_/entries_: order_ хранит ключи в порядке первого claim(), РОВНО те же, что
// живут в entries_, без дубликатов; front — самый старый. Держат его совместно все три мутатора:
//   - claim() кладёт ключ в order_ ТОЛЬКО когда его ещё нет в entries_ (иначе ранний выход);
//   - release() снимает ключ И из entries_, И из order_. Без снятия из order_ (исходный баг 1.3)
//     повторный claim() того же ключа клал бы ВТОРОЙ слот в order_, и evictIfNeeded() затем по
//     устаревшему переднему слоту стёр бы ЖИВУЮ запись -> преждевременная эвикция кэшированного
//     ответа -> дубль отправленного сообщения при ретрае клиента;
//   - evictIfNeeded() снимает самый старый ключ синхронно из обоих контейнеров.
class IdempotencyCache {
   public:
    static constexpr std::size_t kMaxEntries = 1024;

    // max_entries параметризован для юнит-тестов (эвикция без наполнения 1024 записями);
    // в продакшене используется instance() с дефолтом.
    explicit IdempotencyCache(std::size_t max_entries = kMaxEntries) : max_entries_(max_entries) {}

    static IdempotencyCache& instance() {
        static IdempotencyCache cache;
        return cache;
    }

    // 0 = ключ свободен (занят за вами), 1 = в полёте (409), 2 = есть ответ (replay).
    int claim(const std::string& key, int& status, std::string& body) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            evictIfNeeded();
            entries_[key];  // pending
            order_.push_back(key);
            return 0;
        }
        if (!it->second.done) {
            return 1;
        }
        status = it->second.status;
        body = it->second.body;
        return 2;
    }

    void store(const std::string& key, int status, std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(key);
        if (it != entries_.end()) {
            it->second.done = true;
            it->second.status = status;
            it->second.body = std::move(body);
        }
    }

    // При ошибке валидации/апстрима — освобождаем слот, чтобы ретрай прошёл заново.
    void release(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(key);
        // Снять ключ и из order_ (см. инвариант выше): иначе повторный claim() положит дубль.
        for (auto it = order_.begin(); it != order_.end(); ++it) {
            if (*it == key) {
                order_.erase(it);
                break;
            }
        }
    }

   private:
    struct Entry {
        bool done = false;
        int status = 0;
        std::string body;
    };
    void evictIfNeeded() {
        while (order_.size() >= max_entries_ && !order_.empty()) {
            entries_.erase(order_.front());
            order_.pop_front();
        }
    }
    std::size_t max_entries_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::deque<std::string> order_;
};

}  // namespace tgw::http
