#include "http/idempotency_cache.hpp"

#include <gtest/gtest.h>
#include <string>

using tgw::http::IdempotencyCache;

// claim() возвращает: 0 — слот свободен (занят за вами), 1 — запрос в полёте (409),
// 2 — ответ готов (replay). Проверяем полный жизненный цикл ключа.
TEST(IdempotencyCache, ClaimInflightThenReplay) {
    IdempotencyCache cache;
    int status = 0;
    std::string body;

    EXPECT_EQ(cache.claim("K", status, body), 0);  // впервые — слот ваш
    EXPECT_EQ(cache.claim("K", status, body), 1);  // ещё в полёте (нет store) -> 409

    cache.store("K", 200, R"({"ok":true})");

    status = 0;
    body.clear();
    ASSERT_EQ(cache.claim("K", status, body), 2);  // ответ готов -> replay
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, R"({"ok":true})");
}

// release() освобождает слот: повторный claim() того же ключа снова 0 (свободен), а не 1.
// Так ретрай после ошибки валидации/апстрима проходит заново.
TEST(IdempotencyCache, ReleaseFreesSlotForRetry) {
    IdempotencyCache cache;
    int status = 0;
    std::string body;

    EXPECT_EQ(cache.claim("K", status, body), 0);
    cache.release("K");
    EXPECT_EQ(cache.claim("K", status, body), 0);  // снова свободен, не «в полёте»
}

// Эвикция по max_entries: при переполнении вытесняется самый старый ключ (FIFO по первому claim).
TEST(IdempotencyCache, EvictsOldestBeyondCapacity) {
    IdempotencyCache cache(/*max_entries=*/2);
    int status = 0;
    std::string body;

    ASSERT_EQ(cache.claim("k1", status, body), 0);
    cache.store("k1", 200, "b1");
    ASSERT_EQ(cache.claim("k2", status, body), 0);
    cache.store("k2", 200, "b2");
    // Третий ключ вытесняет самый старый (k1).
    ASSERT_EQ(cache.claim("k3", status, body), 0);
    cache.store("k3", 200, "b3");

    EXPECT_EQ(cache.claim("k2", status, body), 2);  // k2 ещё закэширован
    EXPECT_EQ(cache.claim("k1", status, body), 0);  // k1 вытеснен -> слот снова свободен
}

// РЕГРЕССИЯ (баг 1.3, release -> duplicate-slot): release() обязан снимать ключ и из order_,
// а не только из entries_. Иначе повторный claim() того же ключа кладёт ВТОРОЙ слот в order_,
// и evictIfNeeded() по устаревшему переднему слоту стирает ЖИВУЮ запись раньше срока — тогда
// кэшированный ответ теряется и ретрай клиента шлёт сообщение повторно (дубль).
//
// Сценарий: ёмкость 2. claim(A) -> release(A) -> claim(A) снова. С багом order_=[A,A].
// Затем claim(B) на evictIfNeeded видит size()>=2, вытесняет order_.front()==A и убивает
// ЖИВУЮ запись A. Проверяем, что A переживает добавление B и остаётся replay-ответом.
TEST(IdempotencyCache, ReleaseThenReclaimDoesNotCausePrematureEviction) {
    IdempotencyCache cache(/*max_entries=*/2);
    int status = 0;
    std::string body;

    ASSERT_EQ(cache.claim("A", status, body), 0);
    cache.release("A");
    ASSERT_EQ(cache.claim("A", status, body), 0);  // повторный claim после release
    cache.store("A", 201, "created-A");

    // Добавляем второй ключ: с корректным release в order_ ровно один слот A, эвикции нет.
    ASSERT_EQ(cache.claim("B", status, body), 0);

    status = 0;
    body.clear();
    ASSERT_EQ(cache.claim("A", status, body), 2) << "живой A вытеснен преждевременно (баг 1.3)";
    EXPECT_EQ(status, 201);
    EXPECT_EQ(body, "created-A");
}
