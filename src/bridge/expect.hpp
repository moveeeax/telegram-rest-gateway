#pragma once

#include <td/telegram/td_api.h>

#include <utility>

namespace tgw::bridge {

// Проверка типа ответа TDLib (SPEC_ELABORATION §5.10). Статический даункаст error к
// ожидаемому типу без проверки = UB, поэтому идём только через expect<T>.
template <class T>
struct Expected {
    td::td_api::object_ptr<T> value;
    td::td_api::object_ptr<td::td_api::error> error;

    bool ok() const noexcept { return value != nullptr; }
};

template <class T>
Expected<T> expect(td::td_api::object_ptr<td::td_api::Object> object) {
    using namespace td::td_api;
    if (object == nullptr) {
        return {nullptr, make_object<error>(504, "UPSTREAM_TIMEOUT")};
    }
    if (object->get_id() == error::ID) {
        return {nullptr, td::move_tl_object_as<error>(std::move(object))};
    }
    return {td::move_tl_object_as<T>(std::move(object)), nullptr};
}

}  // namespace tgw::bridge
