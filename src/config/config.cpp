#include "config/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace tgw::config {
namespace {

std::string trim(std::string s) {
    const char* ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) {
        return "";
    }
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read secret file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return trim(ss.str());
}

// Приоритет: <NAME>_FILE (секрет из файла) > <NAME> (env) > default.
std::string envOrFile(const char* name, const std::string& fallback = "") {
    const std::string file_var = std::string(name) + "_FILE";
    if (const char* fp = std::getenv(file_var.c_str())) {
        return readFile(fp);
    }
    if (const char* v = std::getenv(name)) {
        return trim(v);
    }
    return fallback;
}

std::string require(const char* name) {
    std::string v = envOrFile(name);
    if (v.empty()) {
        throw std::runtime_error(std::string("missing required config: ") + name + " (set " + name +
                                 " or " + name + "_FILE)");
    }
    return v;
}

// Числовой разбор env-переменной с ИМЕНОВАННОЙ ошибкой (решение 3.5). Раньше числовые значения
// шли через std::stol/stoul/stoull с последующим static_cast: "10GB" молча превращался в 10
// (stoull парсит числовой префикс и игнорирует хвост), а значение больше диапазона типа тихо
// обрезалось при static_cast (например порт 99999 -> 34463) — обе ошибки конфигурации попадали
// в рантайм искажёнными вместо явного отказа на старте. std::from_chars требует, чтобы ВЕСЬ текст
// был числом (иначе ptr != last), и честно сообщает о переполнении (result_out_of_range); имя
// переменной попадает в сообщение, чтобы оператор сразу увидел, что чинить.
template <typename T>
T parseNumericEnv(const char* name, const std::string& value) {
    T out{};
    const char* first = value.data();
    const char* last = value.data() + value.size();
    const auto res = std::from_chars(first, last, out);
    if (res.ec == std::errc::result_out_of_range) {
        throw std::runtime_error(std::string("config variable ") + name +
                                 ": value out of range ('" + value + "')");
    }
    if (res.ec != std::errc{} || res.ptr != last) {
        throw std::runtime_error(std::string("config variable ") + name + ": not a number ('" +
                                 value + "')");
    }
    return out;
}

// Безопасный одиночный сегмент пути S3: [A-Za-z0-9._-], непусто, без '..' целиком.
bool isSafeSegment(const std::string& s) {
    if (s.empty() || s == "." || s == "..") {
        return false;
    }
    for (const unsigned char ch : s) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

Config Config::load() {
    Config c;

    c.api_id = parseNumericEnv<std::int32_t>("API_ID", require("API_ID"));
    c.api_hash = require("API_HASH");
    c.database_encryption_key = require("DATABASE_ENCRYPTION_KEY");
    c.session_b64 = envOrFile("TGW_SESSION");

    c.database_directory = envOrFile("TGW_DATABASE_DIR", c.database_directory);
    c.files_directory = envOrFile("TGW_FILES_DIR", c.files_directory);

    const std::string test_dc = envOrFile("TGW_USE_TEST_DC");
    c.use_test_dc = (test_dc == "1" || test_dc == "true");

    const std::string keep_online = envOrFile("TGW_KEEP_ONLINE");
    c.keep_online = (keep_online == "1" || keep_online == "true");

    // Конфигурация вебхуков mention/reply-событий (TGW_WEBHOOKS_*).
    const std::string webhooks_enabled = envOrFile("TGW_WEBHOOKS_ENABLED");
    c.webhooks_enabled = (webhooks_enabled == "1" || webhooks_enabled == "true");

    const std::string webhook_timeout = envOrFile("TGW_WEBHOOK_TIMEOUT_MS");
    if (!webhook_timeout.empty()) {
        c.webhook_timeout_ms = parseNumericEnv<int>("TGW_WEBHOOK_TIMEOUT_MS", webhook_timeout);
    }

    const std::string webhook_queue = envOrFile("TGW_WEBHOOK_QUEUE_MAX");
    if (!webhook_queue.empty()) {
        c.webhook_queue_max = parseNumericEnv<std::size_t>("TGW_WEBHOOK_QUEUE_MAX", webhook_queue);
    }

    const std::string webhook_ssrf = envOrFile("TGW_WEBHOOK_SSRF_GUARD");
    c.webhook_ssrf_guard = (webhook_ssrf == "1" || webhook_ssrf == "true");

    const std::string verbosity = envOrFile("TGW_TDLIB_LOG_VERBOSITY");
    if (!verbosity.empty()) {
        c.tdlib_log_verbosity = parseNumericEnv<std::int32_t>("TGW_TDLIB_LOG_VERBOSITY", verbosity);
    }

    c.listen_address = envOrFile("TGW_LISTEN_ADDRESS", c.listen_address);
    const std::string port = envOrFile("TGW_LISTEN_PORT");
    if (!port.empty()) {
        c.listen_port = parseNumericEnv<std::uint16_t>("TGW_LISTEN_PORT", port);
    }

    const std::string max_upload = envOrFile("TGW_MAX_UPLOAD_BYTES");
    if (!max_upload.empty()) {
        c.max_upload_bytes = parseNumericEnv<std::size_t>("TGW_MAX_UPLOAD_BYTES", max_upload);
    }

    const std::string max_mem_body = envOrFile("TGW_MAX_MEMORY_BODY_BYTES");
    if (!max_mem_body.empty()) {
        c.max_memory_body_bytes =
            parseNumericEnv<std::size_t>("TGW_MAX_MEMORY_BODY_BYTES", max_mem_body);
    }

    const std::string ws_pending = envOrFile("TGW_WS_MAX_PENDING_BYTES");
    if (!ws_pending.empty()) {
        c.ws_max_pending_bytes =
            parseNumericEnv<std::uint64_t>("TGW_WS_MAX_PENDING_BYTES", ws_pending);
    }

    // Метка инстанса для разграничения сессий в S3. Валидируем как безопасный сегмент пути.
    c.session_id = envOrFile("TGW_SESSION_ID", "default");
    if (!isSafeSegment(c.session_id)) {
        throw std::runtime_error("invalid TGW_SESSION_ID '" + c.session_id +
                                 "': allowed [A-Za-z0-9._-], not '.'/'..'");
    }

    // S3-хранилище сессии. bucket/creds/endpoint пусты — фича выключена (s3.enabled()==false).
    c.s3.endpoint = envOrFile("TGW_S3_ENDPOINT");
    c.s3.region = envOrFile("TGW_S3_REGION", "us-east-1");
    c.s3.bucket = envOrFile("TGW_S3_BUCKET");
    // Ключ объекта: явный TGW_S3_KEY имеет приоритет (полный контроль); иначе строим из
    // TGW_S3_PREFIX/<session_id>/td.binlog — так на одном бакете живёт несколько аккаунтов.
    const std::string explicit_key = envOrFile("TGW_S3_KEY");
    if (!explicit_key.empty()) {
        c.s3.key = explicit_key;
    } else {
        const std::string prefix = envOrFile("TGW_S3_PREFIX", "telegram-sessions");
        c.s3.key = prefix + "/" + c.session_id + "/td.binlog";
    }
    c.s3.access_key = envOrFile("TGW_S3_ACCESS_KEY_ID");
    c.s3.secret_key = envOrFile("TGW_S3_SECRET_ACCESS_KEY");
    const std::string path_style = envOrFile("TGW_S3_PATH_STYLE");
    if (!path_style.empty()) {
        c.s3.path_style = (path_style == "1" || path_style == "true");
    }
    const std::string s3_interval = envOrFile("TGW_S3_SYNC_INTERVAL_SECONDS");
    if (!s3_interval.empty()) {
        c.s3_sync_interval_seconds =
            parseNumericEnv<int>("TGW_S3_SYNC_INTERVAL_SECONDS", s3_interval);
    }

    // Kafka-события: brokers пуст — выключено.
    c.kafka.brokers = envOrFile("TGW_KAFKA_BROKERS");
    c.kafka.topic = envOrFile("TGW_KAFKA_TOPIC", c.kafka.topic);
    c.kafka.client_id = envOrFile("TGW_KAFKA_CLIENT_ID", "tgw-" + c.session_id);

    // Bearer-токены: по строке на токен, #-комментарии и пустые строки игнорируются.
    std::istringstream tokens(envOrFile("BEARER_TOKENS"));
    std::string line;
    while (std::getline(tokens, line)) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') {
            c.bearer_tokens.push_back(line);
        }
    }

    return c;
}

}  // namespace tgw::config
