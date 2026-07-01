#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "bridge/correlation_map.hpp"
#include "bridge/request_state.hpp"

using tgw::bridge::CorrelationMap;
using tgw::bridge::RequestState;
using tgw::bridge::RequestStatePtr;

namespace {

RequestStatePtr makeState(std::uint64_t request_id) {
    auto state = std::make_shared<RequestState>();
    state->request_id = request_id;
    return state;
}

}  // namespace

// Единоличный резолв: два потока гонятся за claim одного request_id — ровно один получает
// узел (SPEC_ELABORATION §5.6, §14.10). Прогонять под TSan.
TEST(CorrelationMap, RaceToClaimHasExactlyOneWinner) {
    constexpr int kRounds = 5000;
    for (int i = 1; i <= kRounds; ++i) {
        CorrelationMap map(/*max_inflight=*/100);
        const auto rid = static_cast<std::uint64_t>(i);
        ASSERT_TRUE(map.tryInsert(makeState(rid)));

        std::atomic<int> winners{0};
        auto claimer = [&] {
            if (map.claim(rid) != nullptr) {
                winners.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(claimer);
        std::thread t2(claimer);
        t1.join();
        t2.join();

        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(map.size(), 0u);
    }
}

// Конкурентные вставки и claim'ы разных request_id: без крашей/гонок, всё изъято.
TEST(CorrelationMap, ConcurrentInsertAndClaim) {
    constexpr std::uint64_t kPerThread = 2000;
    constexpr int kThreads = 4;
    CorrelationMap map(/*max_inflight=*/kThreads * kPerThread + 1);

    std::vector<std::thread> workers;
    std::atomic<int> claimed{0};
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            const auto base = static_cast<std::uint64_t>(t) * kPerThread + 1;
            for (std::uint64_t k = 0; k < kPerThread; ++k) {
                const auto rid = base + k;
                ASSERT_TRUE(map.tryInsert(makeState(rid)));
                if (map.claim(rid) != nullptr) {
                    claimed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    EXPECT_EQ(claimed.load(), kThreads * static_cast<int>(kPerThread));
    EXPECT_EQ(map.size(), 0u);
}

TEST(CorrelationMap, MaxInflightRejectsOverLimit) {
    CorrelationMap map(/*max_inflight=*/2);
    EXPECT_TRUE(map.tryInsert(makeState(1)));
    EXPECT_TRUE(map.tryInsert(makeState(2)));
    EXPECT_FALSE(map.tryInsert(makeState(3)));  // лимит достигнут => 503 на стороне моста
    EXPECT_EQ(map.size(), 2u);
}

TEST(CorrelationMap, ClaimExpiredCollectsOnlyPastDeadline) {
    CorrelationMap map(/*max_inflight=*/10);
    const auto now = std::chrono::steady_clock::now();

    auto fresh = makeState(1);
    fresh->deadline = now + std::chrono::seconds(30);
    auto stale = makeState(2);
    stale->deadline = now - std::chrono::seconds(1);
    ASSERT_TRUE(map.tryInsert(fresh));
    ASSERT_TRUE(map.tryInsert(stale));

    auto expired = map.claimExpired(now);
    ASSERT_EQ(expired.size(), 1u);
    EXPECT_EQ(expired.front()->request_id, 2u);
    EXPECT_EQ(map.size(), 1u);  // fresh остался
}
