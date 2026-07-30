include(FetchContent)

# GoogleTest пинуется по тегу. В CI сеть доступна в builder-стадии; при офлайн-сборке
# использовать заранее подготовленный кэш FetchContent (FETCHCONTENT_SOURCE_DIR_GOOGLETEST).
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(tgw_unit_tests
    transport_smoke_test.cpp
    correlation_map_test.cpp
    auth_state_test.cpp
    token_store_test.cpp
    message_dto_test.cpp
    update_router_test.cpp
    file_dto_test.cpp
    session_io_test.cpp
    scope_policy_test.cpp
    aws_sigv4_test.cpp
    byte_range_test.cpp
    typing_delay_test.cpp
    idempotency_cache_test.cpp
    upload_sanitize_test.cpp
    bearer_auth_test.cpp
    td_bridge_test.cpp
    message_send_tracker_test.cpp
    ws_registry_test.cpp
    owner_mention_detector_test.cpp
    config_test.cpp
    route_smoke_test.cpp
    webhook_registry_test.cpp
    context_builder_test.cpp
    webhook_dispatcher_test.cpp
    # Прод-исходники, которых нет в tgw_bridge, но которые нужны тестам напрямую:
    # config.cpp (парсинг env, тест 3.5), bearer_filter.cpp (реальный фильтр, тест 3.7) и
    # webhook_routes.cpp (handleWebhookCreate, Task 7) — все три собраны в telegram-rest-gateway,
    # а не в tgw_bridge.
    ${PROJECT_SOURCE_DIR}/src/config/config.cpp
    ${PROJECT_SOURCE_DIR}/src/http/bearer_filter.cpp
    ${PROJECT_SOURCE_DIR}/src/http/webhook_routes.cpp)
target_include_directories(tgw_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tgw_unit_tests
    PRIVATE tgw_bridge GTest::gtest_main tgw_project_options)

include(GoogleTest)
gtest_discover_tests(tgw_unit_tests)
