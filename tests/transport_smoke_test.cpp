#include "bridge/transport.hpp"
#include "fake_transport.hpp"

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

namespace td_api = td::td_api;

namespace {

td_api::object_ptr<td_api::Object> makeOk() {
    return td_api::make_object<td_api::ok>();
}

}  // namespace

// Каркасный тест этапа 0: проверяет контракт seam-а транспорта, на котором
// на этапе 1 вырастут TSan/property-тесты корреляции (SPEC_ELABORATION §5.12, §14.9).

TEST(FakeTdTransport, EmptyInboxYieldsTimeout) {
    tgw::testing::FakeTdTransport transport;
    auto resp = transport.receive(0.1);
    EXPECT_EQ(resp.object, nullptr);  // штатный таймаут receive()
    EXPECT_EQ(resp.request_id, 0u);
}

TEST(FakeTdTransport, ResponseRoutedByRequestId) {
    tgw::testing::FakeTdTransport transport;
    const auto cid = transport.createClientId();
    transport.pushResponse(cid, 42, makeOk());

    auto resp = transport.receive(0.1);
    ASSERT_NE(resp.object, nullptr);
    EXPECT_EQ(resp.request_id, 42u);
    EXPECT_EQ(resp.client_id, cid);
    EXPECT_EQ(resp.object->get_id(), td_api::ok::ID);
}

TEST(FakeTdTransport, UpdateHasZeroRequestId) {
    tgw::testing::FakeTdTransport transport;
    const auto cid = transport.createClientId();
    transport.pushUpdate(cid, makeOk());

    auto resp = transport.receive(0.1);
    ASSERT_NE(resp.object, nullptr);
    EXPECT_EQ(resp.request_id, 0u);  // request_id == 0 => апдейт
}

TEST(FakeTdTransport, ClientIdsAreDistinct) {
    tgw::testing::FakeTdTransport transport;
    EXPECT_NE(transport.createClientId(), transport.createClientId());
}

TEST(FakeTdTransport, SendIsRecorded) {
    tgw::testing::FakeTdTransport transport;
    EXPECT_EQ(transport.sentCount(), 0u);
    transport.send(1, 7, td_api::make_object<td_api::getMe>());
    EXPECT_EQ(transport.sentCount(), 1u);
}
