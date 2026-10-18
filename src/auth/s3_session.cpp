#include "auth/s3_session.hpp"

#include "util/aws_sigv4.hpp"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

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

S3SessionSync::S3SessionSync(std::string database_dir, PutFn put)
    : database_dir_(std::move(database_dir)), put_(std::move(put)) {}

bool S3SessionSync::syncOnce() {
    const auto data = readBinlog(database_dir_);
    if (!data) {
        return false;
    }
    std::string hash = tgw::util::sha256Hex(*data);
    if (hash == last_hash_ || !put_(*data)) {
        return false;
    }
    last_hash_ = std::move(hash);
    LOG_INFO << "s3 sync: session pushed (" << data->size() << " bytes)";
    return true;
}

S3SyncHandle startS3Sync(tgw::util::S3Config s3, std::string database_dir, int interval_seconds) {
    S3SyncHandle handle{std::make_shared<std::atomic_bool>(false), [] {}};
    if (!s3.enabled()) {
        return handle;
    }
    auto sync = std::make_shared<S3SessionSync>(
        std::move(database_dir), [s3 = std::move(s3)](const std::string& data) {
            const tgw::util::S3Client client(s3);
            const auto result = client.put(data);
            if (!result.ok()) {
                LOG_WARN << "s3 sync: push failed: http=" << result.http_status << " "
                         << result.error;
            }
            return result.ok();
        });
    // rerun: шаг запрошен, пока шел предыдущий (напр. push_now на Ready во время тика таймера,
    // который мог прочитать binlog еще до авторизации). Поток повторяет шаг, а не теряет запрос.
    auto in_flight = handle.in_flight;
    auto rerun = std::make_shared<std::atomic_bool>(false);
    auto tick = [sync, in_flight, rerun]() {
        // Колбэк исполняется на главном event-loop Drogon. S3Client::send блокирующий,
        // поэтому саму заливку уносим в отдельный поток, чтобы не застопорить loop.
        if (in_flight->exchange(true)) {
            rerun->store(true);
            return;
        }
        std::thread([sync, in_flight, rerun]() {
            for (;;) {
                rerun->store(false);
                sync->syncOnce();
                in_flight->store(false);
                // Запрос, пришедший после store(false), сам займет in_flight и запустит поток.
                if (!rerun->load() || in_flight->exchange(true)) {
                    break;
                }
            }
        }).detach();
    };
    drogon::app().getLoop()->runEvery(static_cast<double>(interval_seconds), tick);
    handle.push_now = [tick]() { drogon::app().getLoop()->queueInLoop(tick); };
    return handle;
}

}  // namespace tgw::auth
