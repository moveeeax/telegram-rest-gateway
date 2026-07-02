#include "config/config.hpp"

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

}  // namespace

Config Config::load() {
    Config c;

    c.api_id = static_cast<std::int32_t>(std::stol(require("API_ID")));
    c.api_hash = require("API_HASH");
    c.database_encryption_key = require("DATABASE_ENCRYPTION_KEY");

    c.database_directory = envOrFile("TGW_DATABASE_DIR", c.database_directory);
    c.files_directory = envOrFile("TGW_FILES_DIR", c.files_directory);

    const std::string test_dc = envOrFile("TGW_USE_TEST_DC");
    c.use_test_dc = (test_dc == "1" || test_dc == "true");

    const std::string verbosity = envOrFile("TGW_TDLIB_LOG_VERBOSITY");
    if (!verbosity.empty()) {
        c.tdlib_log_verbosity = static_cast<std::int32_t>(std::stol(verbosity));
    }

    c.listen_address = envOrFile("TGW_LISTEN_ADDRESS", c.listen_address);
    const std::string port = envOrFile("TGW_LISTEN_PORT");
    if (!port.empty()) {
        c.listen_port = static_cast<std::uint16_t>(std::stoul(port));
    }

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
