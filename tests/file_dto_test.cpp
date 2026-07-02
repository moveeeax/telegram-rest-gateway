#include "dto/file_dto.hpp"

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>
#include <utility>

namespace api = td::td_api;

TEST(FileDto, ReportsProgressAndRemoteId) {
    auto file = api::make_object<api::file>();
    file->id_ = 42;
    file->size_ = 1000;
    file->local_ = api::make_object<api::localFile>();
    file->local_->downloaded_size_ = 250;
    file->local_->is_downloading_completed_ = false;
    file->remote_ = api::make_object<api::remoteFile>();
    file->remote_->unique_id_ = "AQADabc";

    const Json::Value json = tgw::dto::toJson(*file);
    EXPECT_EQ(json["file_id"].asString(), "42");
    EXPECT_EQ(json["size"].asInt64(), 1000);
    EXPECT_EQ(json["downloaded_size"].asInt64(), 250);
    EXPECT_FALSE(json["local_available"].asBool());
    EXPECT_DOUBLE_EQ(json["progress"].asDouble(), 0.25);
    EXPECT_EQ(json["remote_unique_id"].asString(), "AQADabc");
}

TEST(FileDto, ZeroSizeHasZeroProgress) {
    auto file = api::make_object<api::file>();
    file->id_ = 1;
    file->size_ = 0;
    const Json::Value json = tgw::dto::toJson(*file);
    EXPECT_DOUBLE_EQ(json["progress"].asDouble(), 0.0);
}
