#include "auth/session_io.hpp"

#include "util/base64.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using tgw::auth::RestoreResult;

namespace {

std::string uniqueDir(const char* tag) {
    static int counter = 0;
    auto dir = fs::temp_directory_path() /
               (std::string("tgw_session_test_") + tag + std::to_string(++counter));
    fs::remove_all(dir);
    return dir.string();
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(Base64, RoundTrip) {
    const std::string binary("\x00\x01\xFF\x7Fhello \xD0\xBC\xD0\xB8\xD1\x80", 13);
    const auto encoded = tgw::util::base64Encode(binary);
    const auto decoded = tgw::util::base64Decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, binary);
}

TEST(Base64, ToleratesNewlinesRejectsGarbage) {
    EXPECT_EQ(tgw::util::base64Decode("aGVs\nbG8=").value(), "hello");
    EXPECT_FALSE(tgw::util::base64Decode("not!!valid").has_value());
    EXPECT_FALSE(tgw::util::base64Decode("aGVs=x").has_value());  // данные после паддинга
}

TEST(SessionIo, NoSessionWhenEmpty) {
    EXPECT_EQ(tgw::auth::restoreSession(uniqueDir("empty"), ""), RestoreResult::NoSession);
}

TEST(SessionIo, RestoreExportRoundTrip) {
    const auto dir = uniqueDir("roundtrip");
    const std::string binlog("\x01\x02\x03session-bytes\xFF", 17);
    const auto b64 = tgw::util::base64Encode(binlog);

    ASSERT_EQ(tgw::auth::restoreSession(dir, b64), RestoreResult::Restored);
    EXPECT_EQ(readFile(fs::path(dir) / "td.binlog"), binlog);

    const auto exported = tgw::auth::exportSession(dir);
    ASSERT_TRUE(exported.has_value());
    EXPECT_EQ(*exported, b64);
    fs::remove_all(dir);
}

TEST(SessionIo, DoesNotClobberExisting) {
    const auto dir = uniqueDir("existing");
    fs::create_directories(dir);
    std::ofstream(fs::path(dir) / "td.binlog", std::ios::binary) << "live-session";

    const auto b64 = tgw::util::base64Encode("stale-env-session");
    EXPECT_EQ(tgw::auth::restoreSession(dir, b64), RestoreResult::SkippedExisting);
    EXPECT_EQ(readFile(fs::path(dir) / "td.binlog"), "live-session");
    fs::remove_all(dir);
}

TEST(SessionIo, ErrorOnBadBase64) {
    EXPECT_EQ(tgw::auth::restoreSession(uniqueDir("bad"), "!!!"), RestoreResult::Error);
}

TEST(SessionIo, ExportMissingReturnsNullopt) {
    EXPECT_FALSE(tgw::auth::exportSession(uniqueDir("missing")).has_value());
}
