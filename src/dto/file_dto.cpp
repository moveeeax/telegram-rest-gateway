#include "dto/file_dto.hpp"

#include <string>

namespace api = td::td_api;

namespace tgw::dto {

Json::Value toJson(const api::file& file) {
    Json::Value json;
    json["file_id"] = std::to_string(file.id_);
    json["size"] = static_cast<Json::Int64>(file.size_);

    if (file.remote_ != nullptr) {
        json["remote_unique_id"] = file.remote_->unique_id_;
        json["remote_id"] = file.remote_->id_;
    }

    std::int64_t downloaded = 0;
    bool local_available = false;
    if (file.local_ != nullptr) {
        downloaded = file.local_->downloaded_size_;
        local_available = file.local_->is_downloading_completed_;
    }
    json["downloaded_size"] = static_cast<Json::Int64>(downloaded);
    json["local_available"] = local_available;
    json["progress"] =
        (file.size_ > 0) ? static_cast<double>(downloaded) / static_cast<double>(file.size_) : 0.0;
    return json;
}

}  // namespace tgw::dto
