#include "http/upload_cleanup.hpp"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <filesystem>
#include <system_error>

namespace tgw::http {
namespace {

void sweepOnce(const std::string& upload_dir, std::chrono::seconds ttl) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    std::size_t removed = 0;
    for (fs::directory_iterator it(upload_dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        const auto mtime = it->last_write_time(entry_ec);
        if (entry_ec) {
            continue;
        }
        if (now - mtime > ttl) {
            std::error_code rm_ec;
            fs::remove_all(it->path(),
                           rm_ec);  // возврат не суммируем: при ошибке это (uintmax_t)-1
            ++removed;
        }
    }
    if (removed > 0) {
        LOG_INFO << "upload cleanup: removed " << removed << " stale temp entries";
    }
}

}  // namespace

void startUploadCleanup(const std::string& upload_dir, std::chrono::seconds ttl) {
    constexpr double kSweepIntervalSeconds = 600.0;  // раз в 10 минут
    auto* loop = drogon::app().getLoop();
    loop->queueInLoop(
        [upload_dir, ttl] { sweepOnce(upload_dir, ttl); });  // первый проход на старте
    loop->runEvery(kSweepIntervalSeconds, [upload_dir, ttl] { sweepOnce(upload_dir, ttl); });
}

}  // namespace tgw::http
