#include "auth/s3_session.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using tgw::auth::S3SessionSync;

namespace {

std::string uniqueDir(const char* tag) {
    static int counter = 0;
    auto dir = fs::temp_directory_path() /
               (std::string("tgw_s3sync_test_") + tag + std::to_string(++counter));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir.string();
}

void writeBinlog(const std::string& dir, const std::string& data) {
    std::ofstream out(fs::path(dir) / "td.binlog", std::ios::binary | std::ios::trunc);
    out << data;
}

// Фейковый PUT: запоминает залитые данные, результат задается полем ok.
struct FakePut {
    std::vector<std::string> calls;
    bool ok = true;
    S3SessionSync::PutFn fn() {
        return [this](const std::string& data) {
            calls.push_back(data);
            return ok;
        };
    }
};

}  // namespace

TEST(S3SessionSync, FirstSyncPushes) {
    const auto dir = uniqueDir("first");
    writeBinlog(dir, "a");
    FakePut put;
    S3SessionSync sync(dir, put.fn());
    EXPECT_TRUE(sync.syncOnce());
    ASSERT_EQ(put.calls.size(), 1U);
    EXPECT_EQ(put.calls[0], "a");
}

TEST(S3SessionSync, UnchangedBinlogSkipped) {
    const auto dir = uniqueDir("same");
    writeBinlog(dir, "a");
    FakePut put;
    S3SessionSync sync(dir, put.fn());
    ASSERT_TRUE(sync.syncOnce());
    EXPECT_FALSE(sync.syncOnce());
    EXPECT_EQ(put.calls.size(), 1U);
}

TEST(S3SessionSync, ChangedBinlogPushed) {
    const auto dir = uniqueDir("changed");
    writeBinlog(dir, "a");
    FakePut put;
    S3SessionSync sync(dir, put.fn());
    ASSERT_TRUE(sync.syncOnce());
    writeBinlog(dir, "b");
    EXPECT_TRUE(sync.syncOnce());
    ASSERT_EQ(put.calls.size(), 2U);
    EXPECT_EQ(put.calls[1], "b");
}

TEST(S3SessionSync, FailedPutIsRetried) {
    const auto dir = uniqueDir("retry");
    writeBinlog(dir, "a");
    FakePut put;
    put.ok = false;
    S3SessionSync sync(dir, put.fn());
    EXPECT_FALSE(sync.syncOnce());
    put.ok = true;
    EXPECT_TRUE(sync.syncOnce());
    EXPECT_EQ(put.calls.size(), 2U);
}

TEST(S3SessionSync, MissingBinlogNoPut) {
    const auto dir = uniqueDir("missing");
    FakePut put;
    S3SessionSync sync(dir, put.fn());
    EXPECT_FALSE(sync.syncOnce());
    EXPECT_TRUE(put.calls.empty());
}

using tgw::auth::SyncGate;

TEST(SyncGate, FirstRequestStartsWorker) {
    SyncGate gate;
    EXPECT_TRUE(gate.tryStart());
    EXPECT_TRUE(gate.inFlight()->load());
}

TEST(SyncGate, RequestWhileRunningIsCoalescedNotLost) {
    SyncGate gate;
    ASSERT_TRUE(gate.tryStart());
    EXPECT_FALSE(gate.tryStart());  // поток уже идет: запрос откладывается
    EXPECT_TRUE(gate.finishStep());  // отложенный запрос: еще один шаг
    EXPECT_TRUE(gate.inFlight()->load());  // между шагами in_flight не сбрасывается
    EXPECT_FALSE(gate.finishStep());  // больше запросов нет: поток выходит
    EXPECT_FALSE(gate.inFlight()->load());
}

TEST(SyncGate, SeveralRequestsWhileRunningGiveOneExtraStep) {
    SyncGate gate;
    ASSERT_TRUE(gate.tryStart());
    EXPECT_FALSE(gate.tryStart());
    EXPECT_FALSE(gate.tryStart());
    EXPECT_TRUE(gate.finishStep());
    EXPECT_FALSE(gate.finishStep());
}

TEST(SyncGate, RestartAfterFinish) {
    SyncGate gate;
    ASSERT_TRUE(gate.tryStart());
    ASSERT_FALSE(gate.finishStep());
    EXPECT_TRUE(gate.tryStart());
}

// После logout бинлог в S3 трогать нельзя: TDLib стер локальные данные, а в S3 лежит снимок до
// logout, который удалит control plane. disable() выключает все последующие PUT.
TEST(S3SessionSync, DisableStopsPuts) {
    const auto dir = uniqueDir("disable");
    writeBinlog(dir, "a");
    FakePut put;
    S3SessionSync sync(dir, put.fn());
    sync.disable();
    EXPECT_FALSE(sync.syncOnce());
    writeBinlog(dir, "b");
    EXPECT_FALSE(sync.syncOnce());
    EXPECT_TRUE(put.calls.empty());
}
