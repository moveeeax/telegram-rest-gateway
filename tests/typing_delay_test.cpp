#include "http/typing_delay.hpp"

#include <gtest/gtest.h>

using tgw::http::computeTypingDelay;
using tgw::http::TypingDelayParams;

TEST(TypingDelay, MidJitterSampleGivesBaseValue) {
    TypingDelayParams p{200, 20, 1000, 10000};
    // 200 симв/мин = 1 символ/300мс; 60 символов -> 18000мс -> clamp к max 10000.
    auto d = computeTypingDelay(60, p, 0.5);
    EXPECT_EQ(d.count(), 10000);
}

TEST(TypingDelay, ShortTextClampsToMin) {
    TypingDelayParams p{200, 20, 1000, 10000};
    auto d = computeTypingDelay(1, p, 0.5);
    EXPECT_EQ(d.count(), 1000);
}

TEST(TypingDelay, JitterSampleZeroGivesLowerBound) {
    TypingDelayParams p{6000, 0, 0, 100000};  // 6000/мин = 100/сек = 10мс/символ
    auto low = computeTypingDelay(100, p, 0.0);  // base=1000мс, jitter=0 -> всегда 1000
    EXPECT_EQ(low.count(), 1000);
}

TEST(TypingDelay, JitterWidensRange) {
    TypingDelayParams p{6000, 20, 0, 100000};  // base для 100 символов = 1000мс
    auto low = computeTypingDelay(100, p, 0.0);
    auto high = computeTypingDelay(100, p, 1.0);
    EXPECT_EQ(low.count(), 800);    // 1000*(1-0.2)
    EXPECT_EQ(high.count(), 1200);  // 1000*(1+0.2)
}

TEST(TypingDelay, ZeroLengthGivesMin) {
    TypingDelayParams p{200, 20, 1000, 10000};
    EXPECT_EQ(computeTypingDelay(0, p, 0.5).count(), 1000);
}
