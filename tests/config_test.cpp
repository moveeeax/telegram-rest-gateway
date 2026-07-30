// setenv/unsetenv — POSIX, а не ISO C: под -std=c++20 (__STRICT_ANSI__) glibc прячет их без
// явного запроса набора по умолчанию. Просим _DEFAULT_SOURCE до любых включений.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "config/config.hpp"

#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

using tgw::config::Config;

namespace {

// Все базовые имена переменных, которые читает Config::load. Для каждого чистим и <NAME>, и
// <NAME>_FILE (envOrFile проверяет обе), чтобы тест не зависел ни от окружения хоста, ни от
// порядка запуска. Исходные значения сохраняются и восстанавливаются в TearDown.
constexpr const char* kEnvBases[] = {
    "API_ID",
    "API_HASH",
    "DATABASE_ENCRYPTION_KEY",
    "TGW_SESSION",
    "TGW_DATABASE_DIR",
    "TGW_FILES_DIR",
    "TGW_USE_TEST_DC",
    "TGW_KEEP_ONLINE",
    "TGW_KEEP_ONLINE_INTERVAL_SECONDS",
    "TGW_TDLIB_LOG_VERBOSITY",
    "TGW_LISTEN_ADDRESS",
    "TGW_LISTEN_PORT",
    "TGW_MAX_UPLOAD_BYTES",
    "TGW_MAX_MEMORY_BODY_BYTES",
    "TGW_WS_MAX_PENDING_BYTES",
    "TGW_SESSION_ID",
    "TGW_S3_ENDPOINT",
    "TGW_S3_REGION",
    "TGW_S3_BUCKET",
    "TGW_S3_KEY",
    "TGW_S3_PREFIX",
    "TGW_S3_ACCESS_KEY_ID",
    "TGW_S3_SECRET_ACCESS_KEY",
    "TGW_S3_PATH_STYLE",
    "TGW_S3_SYNC_INTERVAL_SECONDS",
    "TGW_KAFKA_BROKERS",
    "TGW_KAFKA_TOPIC",
    "TGW_KAFKA_CLIENT_ID",
    "BEARER_TOKENS",
};

class ConfigTest : public ::testing::Test {
   protected:
    void SetUp() override {
        for (const char* base : kEnvBases) {
            snapshotAndClear(base);
            snapshotAndClear(std::string(base) + "_FILE");
        }
    }

    void TearDown() override {
        for (const auto& [name, value] : saved_) {
            if (value) {
                ::setenv(name.c_str(), value->c_str(), 1);
            } else {
                ::unsetenv(name.c_str());
            }
        }
    }

    // Задаёт минимальный набор обязательных переменных для успешной загрузки.
    static void setRequired() {
        ::setenv("API_ID", "12345", 1);
        ::setenv("API_HASH", "test-api-hash", 1);
        ::setenv("DATABASE_ENCRYPTION_KEY", "test-key", 1);
    }

    static void set(const char* name, const char* value) { ::setenv(name, value, 1); }

   private:
    void snapshotAndClear(const std::string& name) {
        if (saved_.count(name) != 0) {
            return;
        }
        const char* current = std::getenv(name.c_str());
        saved_[name] = current ? std::optional<std::string>(current) : std::nullopt;
        ::unsetenv(name.c_str());
    }

    std::map<std::string, std::optional<std::string>> saved_;
};

}  // namespace

// Обязательные переменные заданы, остальные — дефолты из Config.
TEST_F(ConfigTest, LoadsRequiredWithDefaults) {
    setRequired();
    const Config c = Config::load();
    EXPECT_EQ(c.api_id, 12345);
    EXPECT_EQ(c.api_hash, "test-api-hash");
    EXPECT_EQ(c.database_encryption_key, "test-key");
    EXPECT_EQ(c.listen_port, 8080);      // дефолт
    EXPECT_EQ(c.session_id, "default");  // дефолт
}

// Отсутствие обязательной переменной -> ошибка с её именем в сообщении.
TEST_F(ConfigTest, MissingRequiredThrowsNamedError) {
    set("API_HASH", "h");
    set("DATABASE_ENCRYPTION_KEY", "k");  // API_ID не задан
    try {
        Config::load();
        FAIL() << "ожидалось исключение об отсутствии API_ID";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("API_ID"), std::string::npos);
    }
}

// Индирекция через <NAME>_FILE: значение читается из файла (с trim) и имеет приоритет над <NAME>.
TEST_F(ConfigTest, FileIndirectionTakesPrecedence) {
    const std::string path = std::string(::testing::TempDir()) + "/api_hash_secret";
    {
        std::ofstream out(path);
        out << "  hash-from-file\n";  // ведущие/замыкающие пробелы обязаны обрезаться
    }
    set("API_ID", "1");
    set("DATABASE_ENCRYPTION_KEY", "k");
    set("API_HASH", "hash-from-env");  // должен проиграть файлу
    ::setenv("API_HASH_FILE", path.c_str(), 1);

    const Config c = Config::load();
    EXPECT_EQ(c.api_hash, "hash-from-file");
}

// isSafeSegment: '..' и '/' в TGW_SESSION_ID отвергаются (защита пути в S3), валидное — проходит.
TEST_F(ConfigTest, SessionIdRejectsUnsafeSegments) {
    setRequired();
    set("TGW_SESSION_ID", "..");
    EXPECT_THROW(Config::load(), std::runtime_error);

    set("TGW_SESSION_ID", "a/b");
    EXPECT_THROW(Config::load(), std::runtime_error);

    set("TGW_SESSION_ID", "acct-1_v.2");
    Config c = Config::load();
    EXPECT_EQ(c.session_id, "acct-1_v.2");
}

// Нечисловое значение числовой переменной -> ИМЕНОВАННАЯ ошибка (раньше "10GB" молча давал 10).
TEST_F(ConfigTest, NonNumericValueThrowsNamedError) {
    setRequired();
    set("TGW_MAX_UPLOAD_BYTES", "10GB");
    try {
        Config::load();
        FAIL() << "ожидалось исключение на нечисловом TGW_MAX_UPLOAD_BYTES";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("TGW_MAX_UPLOAD_BYTES"), std::string::npos);
    }
}

// Нечисловой API_ID тоже даёт именованную ошибку (проходит через тот же конвертер).
TEST_F(ConfigTest, NonNumericApiIdThrowsNamedError) {
    set("API_ID", "not-a-number");
    set("API_HASH", "h");
    set("DATABASE_ENCRYPTION_KEY", "k");
    try {
        Config::load();
        FAIL() << "ожидалось исключение на нечисловом API_ID";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("API_ID"), std::string::npos);
    }
}

// Значение вне диапазона типа -> ИМЕНОВАННАЯ ошибка (раньше 99999 тихо обрезался до uint16_t).
TEST_F(ConfigTest, OutOfRangeValueThrowsNamedError) {
    setRequired();
    set("TGW_LISTEN_PORT", "99999");  // > 65535
    try {
        Config::load();
        FAIL() << "ожидалось исключение на выходящем за диапазон TGW_LISTEN_PORT";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("TGW_LISTEN_PORT"), std::string::npos);
    }
}

// Границы uint16_t для TGW_LISTEN_PORT: 65535 валиден; 65536 — именованная ошибка; 0 принимается
// (from_chars парсит "0" в 0 — валидное значение типа; семантику «порт 0» load() не трактует,
// фиксируем фактическое поведение: не отвергает).
TEST_F(ConfigTest, ListenPortBoundaries) {
    setRequired();

    set("TGW_LISTEN_PORT", "65535");
    {
        const Config c = Config::load();
        EXPECT_EQ(c.listen_port, 65535);
    }

    set("TGW_LISTEN_PORT", "65536");
    try {
        Config::load();
        FAIL() << "65536 вне диапазона uint16_t — ожидалась ошибка";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("TGW_LISTEN_PORT"), std::string::npos);
    }

    set("TGW_LISTEN_PORT", "0");
    {
        const Config c = Config::load();
        EXPECT_EQ(c.listen_port, 0);  // 0 — валидный uint16_t; load() не отвергает и не трактует
    }
}

// TGW_KEEP_ONLINE: "1"/"true" → true; отсутствие/"0"/"false" → false (образец use_test_dc).
TEST_F(ConfigTest, KeepOnlineFlagParsing) {
    setRequired();
    {
        const Config c = Config::load();
        EXPECT_FALSE(c.keep_online);  // не задано — дефолт false
    }
    set("TGW_KEEP_ONLINE", "1");
    EXPECT_TRUE(Config::load().keep_online);
    set("TGW_KEEP_ONLINE", "true");
    EXPECT_TRUE(Config::load().keep_online);
    set("TGW_KEEP_ONLINE", "0");
    EXPECT_FALSE(Config::load().keep_online);
    set("TGW_KEEP_ONLINE", "false");
    EXPECT_FALSE(Config::load().keep_online);
    set("TGW_KEEP_ONLINE", "yes");  // любое иное значение трактуется как false
    EXPECT_FALSE(Config::load().keep_online);
}

// TGW_KEEP_ONLINE_INTERVAL_SECONDS: дефолт 60, валидное значение парсится (образец
// s3_sync_interval_seconds/TGW_S3_SYNC_INTERVAL_SECONDS).
TEST_F(ConfigTest, KeepOnlineIntervalParsing) {
    setRequired();
    {
        const Config c = Config::load();
        EXPECT_EQ(c.keep_online_interval_seconds, 60);  // не задано — дефолт
    }
    set("TGW_KEEP_ONLINE_INTERVAL_SECONDS", "30");
    EXPECT_EQ(Config::load().keep_online_interval_seconds, 30);
}

// Валидные числовые значения парсятся без изменения поведения.
TEST_F(ConfigTest, ValidNumericValuesParsed) {
    setRequired();
    set("TGW_LISTEN_PORT", "9090");
    set("TGW_MAX_UPLOAD_BYTES", "1048576");
    set("TGW_WS_MAX_PENDING_BYTES", "2048");
    set("TGW_TDLIB_LOG_VERBOSITY", "3");

    const Config c = Config::load();
    EXPECT_EQ(c.listen_port, 9090);
    EXPECT_EQ(c.max_upload_bytes, 1048576u);
    EXPECT_EQ(c.ws_max_pending_bytes, 2048u);
    EXPECT_EQ(c.tdlib_log_verbosity, 3);
}
