#include "auth/session_io.hpp"

#include "util/base64.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace tgw::auth {
namespace {

std::filesystem::path binlogPath(const std::string& database_dir) {
    return std::filesystem::path(database_dir) / "td.binlog";
}

}  // namespace

RestoreResult restoreSession(const std::string& database_dir, const std::string& session_b64) {
    if (session_b64.empty()) {
        return RestoreResult::NoSession;
    }
    const auto path = binlogPath(database_dir);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return RestoreResult::SkippedExisting;
    }
    const auto decoded = tgw::util::base64Decode(session_b64);
    if (!decoded || decoded->empty()) {
        return RestoreResult::Error;
    }
    std::filesystem::create_directories(database_dir, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return RestoreResult::Error;
    }
    out.write(decoded->data(), static_cast<std::streamsize>(decoded->size()));
    out.close();
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    return out.good() ? RestoreResult::Restored : RestoreResult::Error;
}

std::optional<std::string> exportSession(const std::string& database_dir) {
    std::ifstream in(binlogPath(database_dir), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string bytes = buffer.str();
    if (bytes.empty()) {
        return std::nullopt;
    }
    return tgw::util::base64Encode(bytes);
}

}  // namespace tgw::auth
