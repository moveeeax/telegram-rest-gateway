#include "auth/startup_bootstrapper.hpp"

#include "auth/auth_state_manager.hpp"
#include "bridge/td_bridge.hpp"
#include "config/config.hpp"

#include <td/telegram/td_api.h>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <string>

namespace tgw::auth {

namespace api = td::td_api;
using namespace std::chrono_literals;

namespace {

api::object_ptr<api::setTdlibParameters> buildParameters(const tgw::config::Config& c) {
    auto p = api::make_object<api::setTdlibParameters>();
    p->use_test_dc_ = c.use_test_dc;
    p->database_directory_ = c.database_directory;
    p->files_directory_ = c.files_directory;
    p->database_encryption_key_ = c.database_encryption_key;
    // Stateless-режим: кэш-БД отключены — единственный state это td.binlog (auth key,
    // десятки КБ), целиком помещается в session string (TGW_SESSION). История и чаты
    // тянутся с сервера (getChatHistory идёт с only_local=false).
    p->use_file_database_ = false;
    p->use_chat_info_database_ = false;
    p->use_message_database_ = false;
    p->use_secret_chats_ = false;
    p->api_id_ = c.api_id;
    p->api_hash_ = c.api_hash;
    p->system_language_code_ = "en";
    p->device_model_ = "telegram-rest-gateway";
    p->system_version_ = "";
    p->application_version_ = c.application_version;
    return p;
}

}  // namespace

bool StartupBootstrapper::run(tgw::bridge::TdBridge& bridge, std::int32_t client_id,
                              const tgw::config::Config& config, AuthStateManager& auth) {
    // Лог TDLib уже заглушён синхронно в main (RealTdTransport::configureGlobalLogging, §13).
    // Толкаем поток апдейтов: первый запрос инициирует updateAuthorizationState.
    bridge.sendOneWay(client_id, api::make_object<api::getOption>("version"));

    if (!auth.waitForFirst(10s)) {
        LOG_ERROR << "bootstrap: no updateAuthorizationState within timeout";
        return false;
    }

    if (auth.current() == AuthState::WaitTdlibParameters) {
        const std::uint64_t gen = auth.generation();
        bridge.sendOneWay(client_id, buildParameters(config));
        if (!auth.waitForChange(gen, 15s)) {
            // Частая причина — рассинхрон database_encryption_key/api-кред с существующей БД
            // (§7.6).
            LOG_ERROR << "bootstrap: setTdlibParameters produced no state transition "
                         "(possible DB key / api credentials mismatch)";
            return false;
        }
    }

    LOG_INFO << "bootstrap: authorization_state=" << std::string(toString(auth.current()));
    return true;
}

}  // namespace tgw::auth
