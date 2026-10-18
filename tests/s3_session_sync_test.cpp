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
