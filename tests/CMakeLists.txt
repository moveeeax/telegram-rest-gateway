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
    aws_sigv4_test.cpp)
target_include_directories(tgw_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tgw_unit_tests
    PRIVATE tgw_bridge GTest::gtest_main tgw_project_options)

include(GoogleTest)
gtest_discover_tests(tgw_unit_tests)
