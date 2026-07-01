cmake_minimum_required(VERSION 3.24)

# Версия сервиса (идёт в application_version setTdlibParameters и X-TDLib-Version).
project(telegram-rest-gateway
    VERSION 0.0.1
    LANGUAGES CXX)

# --- Стандарт: C++20 на НАШИХ таргетах (корутины Drogon). TDLib собирается своим C++17. ---
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Опции ---
option(TGW_BUILD_TESTS "Build unit tests" ON)
option(TGW_ENABLE_ASAN "AddressSanitizer + UBSan" OFF)
option(TGW_ENABLE_TSAN "ThreadSanitizer" OFF)

# --- Зависимости ---
# TDLib: нативный C++-API доступен ТОЛЬКО через статический Td::TdStatic
# (td::ClientManager / td_api::* НЕ экспортируются libtdjson.so). См. docs/SPEC_ELABORATION.md §11.1.
find_package(Td CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
# Единый OpenSSL для TDLib, Drogon/Trantor и нашего кода (§11.2).
find_package(OpenSSL 3.0 REQUIRED)

# --- Предупреждения и санитайзеры только на НАШИХ таргетах ---
add_library(tgw_project_options INTERFACE)
target_compile_options(tgw_project_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(TGW_ENABLE_ASAN AND TGW_ENABLE_TSAN)
    message(FATAL_ERROR "ASAN и TSAN несовместимы — включай по одному")
endif()
if(TGW_ENABLE_ASAN)
    # -fno-sanitize=vptr: UBSan vptr-чек требует RTTI/typeinfo, которых нет у типов
    # td::td_api::* (TDLib собран без RTTI) — иначе undefined reference при линковке.
    target_compile_options(tgw_project_options INTERFACE
        -fsanitize=address,undefined -fno-sanitize=vptr -fno-omit-frame-pointer)
    target_link_options(tgw_project_options INTERFACE
        -fsanitize=address,undefined -fno-sanitize=vptr)
endif()
if(TGW_ENABLE_TSAN)
    target_compile_options(tgw_project_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(tgw_project_options INTERFACE -fsanitize=thread)
endif()

# --- tgw_bridge: ядро моста (порт транспорта, корреляция, поток-приёмник). ---
# Использует td_api::* (Td::TdStatic) и trantor EventLoop::queueInLoop + логгер (Drogon).
add_library(tgw_bridge STATIC
    src/bridge/real_transport.cpp
    src/bridge/correlation_map.cpp
    src/bridge/update_sink.cpp
    src/bridge/td_bridge.cpp)
target_include_directories(tgw_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tgw_bridge
    PUBLIC Td::TdStatic Drogon::Drogon
    PRIVATE tgw_project_options)

# --- Исполняемый сервис ---
add_executable(telegram-rest-gateway
    src/main.cpp
    src/http/routes.cpp)
target_link_libraries(telegram-rest-gateway
    PRIVATE tgw_bridge Drogon::Drogon tgw_project_options)

# --- Тесты ---
if(TGW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
