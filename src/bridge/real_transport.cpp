#include "bridge/real_transport.hpp"

#include <utility>

namespace tgw::bridge {

RealTdTransport::RealTdTransport() : manager_(std::make_unique<td::ClientManager>()) {}

void RealTdTransport::configureGlobalLogging(std::int32_t verbosity) {
    // setLogVerbosityLevel — синхронная функция; execute применяет её немедленно и глобально.
    td::ClientManager::execute(
        td::td_api::make_object<td::td_api::setLogVerbosityLevel>(verbosity));
}

std::int32_t RealTdTransport::createClientId() {
    return manager_->create_client_id();
}

void RealTdTransport::send(std::int32_t client_id, std::uint64_t request_id,
                           td::td_api::object_ptr<td::td_api::Function> fn) {
    manager_->send(client_id, request_id, std::move(fn));
}

Response RealTdTransport::receive(double timeout_seconds) {
    td::ClientManager::Response raw = manager_->receive(timeout_seconds);
    return Response{raw.client_id, raw.request_id, std::move(raw.object)};
}

}  // namespace tgw::bridge
