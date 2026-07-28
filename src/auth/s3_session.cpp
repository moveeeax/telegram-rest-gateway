#include "auth/s3_session.hpp"

#include "util/aws_sigv4.hpp"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace tgw::auth {
namespace {

std::filesystem::path binlogPath(const std::string& database_dir) {
    return std::filesystem::path(database_dir) / "td.binlog";
}

// Пишет data в path файлом СРАЗУ с правами 0600. td.binlog содержит auth-ключ Telegram —
// он не должен ни на миг быть world-readable, поэтому создаём файл через ::open с mode 0600,
// а не через ofstream (umask -> 0644) с последующим chmod. fchmod форсирует ровно 0600: mode в
// open() маскируется umask, а O_TRUNC над уже существующим файлом его права вообще не меняет.
// false — при любой ошибке open/fchmod/write (частичная запись = провал восстановления).
bool writeSecretFile(const std::filesystem::path& path, const char* data, std::size_t size) {
    int fd = -1;
    do {
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        return false;
    }
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // прерывание сигналом — повторяем тот же кусок
            }
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    // close() может отдать отложенную ошибку записи — не игнорируем.
    return ::close(fd) == 0;
}

std::optional<std::string> readBinlog(const std::string& database_dir) {
    std::ifstream in(binlogPath(database_dir), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string bytes = buffer.str();
    if (bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

}  // namespace

S3RestoreResult restoreFromS3(const tgw::util::S3Config& s3, const std::string& database_dir) {
    if (!s3.enabled()) {
        return S3RestoreResult::NotConfigured;
    }
    std::error_code ec;
    if (std::filesystem::exists(binlogPath(database_dir), ec)) {
        return S3RestoreResult::SkippedExisting;
    }
    const tgw::util::S3Client client(s3);
    const auto result = client.get();
    if (result.notFound()) {
        return S3RestoreResult::NoRemoteObject;
    }
    if (!result.ok()) {
        LOG_ERROR << "s3 restore failed: http=" << result.http_status << " " << result.error;
        return S3RestoreResult::Error;
    }
    std::filesystem::create_directories(database_dir, ec);
    // Неполная запись (напр. disk full) или провал прав => не Restored: секрет не должен
    // остаться частичным или world-readable, честно сообщаем о провале восстановления.
    if (!writeSecretFile(binlogPath(database_dir), result.body.data(), result.body.size())) {
        LOG_ERROR << "s3 restore: failed to write td.binlog with 0600 perms";
        return S3RestoreResult::Error;
    }
    return S3RestoreResult::Restored;
}

bool pushToS3(const tgw::util::S3Config& s3, const std::string& database_dir) {
    if (!s3.enabled()) {
        return false;
    }
    const auto data = readBinlog(database_dir);
    if (!data) {
        return false;
    }
    const tgw::util::S3Client client(s3);
    const auto result = client.put(*data);
    if (!result.ok()) {
        LOG_ERROR << "s3 push failed: http=" << result.http_status << " " << result.error;
    }
    return result.ok();
}

std::shared_ptr<std::atomic_bool> startS3Sync(tgw::util::S3Config s3, std::string database_dir,
                                              int interval_seconds) {
    auto in_flight = std::make_shared<std::atomic_bool>(false);
    if (!s3.enabled()) {
        return in_flight;
    }
    auto last_hash = std::make_shared<std::string>();
    drogon::app().getLoop()->runEvery(
        static_cast<double>(interval_seconds),
        [s3 = std::move(s3), database_dir = std::move(database_dir), last_hash, in_flight]() {
            // Колбэк исполняется на главном event-loop Drogon. S3Client::send блокирующий,
            // поэтому саму заливку уносим в отдельный поток, чтобы не застопорить loop.
            if (in_flight->exchange(true)) {
                return;  // предыдущий push ещё идёт
            }
            std::thread([s3, database_dir, last_hash, in_flight]() {
                const auto data = readBinlog(database_dir);
                if (data) {
                    const std::string hash = tgw::util::sha256Hex(*data);
                    if (*last_hash != hash) {  // изменился — заливаем
                        const tgw::util::S3Client client(s3);
                        if (client.put(*data).ok()) {
                            *last_hash = hash;
                            LOG_INFO << "s3 sync: session pushed (" << data->size() << " bytes)";
                        }
                    }
                }
                in_flight->store(false);
            }).detach();
        });
    return in_flight;
}

}  // namespace tgw::auth
