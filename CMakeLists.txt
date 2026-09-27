cmake_minimum_required(VERSION 3.24)
project(MiniCloud VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(MINICLOUD_BUILD_TESTS "Build MiniCloud tests" ON)
option(MINICLOUD_WARNINGS_AS_ERRORS "Treat warnings in MiniCloud sources as errors" ON)

find_package(Threads REQUIRED)
find_package(Boost 1.74 REQUIRED COMPONENTS system)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Protobuf CONFIG QUIET)
if(NOT TARGET protobuf::libprotobuf OR NOT TARGET protobuf::protoc)
  find_package(Protobuf MODULE REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)
find_package(libpqxx 7.9...<8 CONFIG QUIET)
if(TARGET libpqxx::pqxx)
  set(MINICLOUD_PQXX_TARGET libpqxx::pqxx)
elseif(TARGET pqxx)
  set(MINICLOUD_PQXX_TARGET pqxx)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PQXX REQUIRED IMPORTED_TARGET "libpqxx>=7.9")
  if(PQXX_VERSION VERSION_GREATER_EQUAL 8)
    message(FATAL_ERROR "MiniCloud currently requires libpqxx 7.9.x")
  endif()
  set(MINICLOUD_PQXX_TARGET PkgConfig::PQXX)
endif()
find_package(hiredis CONFIG QUIET)
if(TARGET hiredis::hiredis)
  set(MINICLOUD_HIREDIS_TARGET hiredis::hiredis)
elseif(TARGET hiredis)
  set(MINICLOUD_HIREDIS_TARGET hiredis)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
  set(MINICLOUD_HIREDIS_TARGET PkgConfig::HIREDIS)
endif()

function(minicloud_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus
                           /utf-8 /external:anglebrackets /external:W0)
    if(MINICLOUD_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
    if(MINICLOUD_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/controller_worker.proto")
set(PROTO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GENERATED_DIR}")
set(PROTO_SOURCE "${PROTO_GENERATED_DIR}/controller_worker.pb.cc")
set(PROTO_HEADER "${PROTO_GENERATED_DIR}/controller_worker.pb.h")
set(GRPC_SOURCE "${PROTO_GENERATED_DIR}/controller_worker.grpc.pb.cc")
set(GRPC_HEADER "${PROTO_GENERATED_DIR}/controller_worker.grpc.pb.h")
find_path(MINICLOUD_PROTOBUF_PROTO_INCLUDE_DIR
  NAMES google/protobuf/timestamp.proto
  HINTS ${Protobuf_INCLUDE_DIRS}
)
if(NOT MINICLOUD_PROTOBUF_PROTO_INCLUDE_DIR)
  message(FATAL_ERROR "Could not locate the protobuf well-known .proto include directory")
endif()

add_custom_command(
  OUTPUT "${PROTO_SOURCE}" "${PROTO_HEADER}" "${GRPC_SOURCE}" "${GRPC_HEADER}"
  COMMAND $<TARGET_FILE:protobuf::protoc>
  ARGS --cpp_out=${PROTO_GENERATED_DIR}
       --grpc_out=${PROTO_GENERATED_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
       --proto_path=${MINICLOUD_PROTOBUF_PROTO_INCLUDE_DIR}
       ${PROTO_FILE}
  DEPENDS "${PROTO_FILE}" protobuf::protoc gRPC::grpc_cpp_plugin
  VERBATIM
)

add_library(minicloud_proto STATIC "${PROTO_SOURCE}" "${GRPC_SOURCE}")
target_include_directories(minicloud_proto PUBLIC "${PROTO_GENERATED_DIR}")
target_link_libraries(minicloud_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
# Generated protobuf/gRPC sources are third-party output and intentionally do
# not inherit the project's warnings-as-errors policy.

add_library(minicloud_core STATIC
  cpp/core/domain.cpp
  cpp/core/scheduler.cpp
  cpp/core/reconciler.cpp
)
target_include_directories(minicloud_core PUBLIC cpp/include)
minicloud_warnings(minicloud_core)

add_library(minicloud_common STATIC
  cpp/common/config.cpp
  cpp/common/http_server.cpp
  cpp/common/metrics.cpp
)
target_include_directories(minicloud_common PUBLIC cpp/include)
target_link_libraries(minicloud_common PUBLIC Boost::system Threads::Threads)
minicloud_warnings(minicloud_common)

add_library(minicloud_runtime STATIC
  cpp/runtime/docker_client.cpp
  cpp/runtime/worker_runtime.cpp
  cpp/runtime/metrics.cpp
)
target_include_directories(minicloud_runtime PUBLIC cpp/include)
target_link_libraries(minicloud_runtime PUBLIC CURL::libcurl nlohmann_json::nlohmann_json Threads::Threads)
minicloud_warnings(minicloud_runtime)

add_library(minicloud_valkey STATIC cpp/controller/valkey_store.cpp)
target_include_directories(minicloud_valkey PUBLIC cpp/include)
target_link_libraries(minicloud_valkey PUBLIC ${MINICLOUD_HIREDIS_TARGET} nlohmann_json::nlohmann_json)
minicloud_warnings(minicloud_valkey)

add_library(minicloud_controller_lib STATIC
  cpp/controller/repository.cpp
  cpp/controller/api.cpp
  cpp/controller/grpc_service.cpp
)
target_include_directories(minicloud_controller_lib PUBLIC cpp/include)
target_link_libraries(minicloud_controller_lib PUBLIC
  minicloud_common minicloud_core minicloud_proto minicloud_valkey
  ${MINICLOUD_PQXX_TARGET} nlohmann_json::nlohmann_json
)
minicloud_warnings(minicloud_controller_lib)

add_executable(minicloud-controller cpp/controller/main.cpp)
target_link_libraries(minicloud-controller PRIVATE minicloud_controller_lib)
minicloud_warnings(minicloud-controller)

add_executable(minicloud-worker cpp/worker/main.cpp)
target_link_libraries(minicloud-worker PRIVATE minicloud_common minicloud_runtime minicloud_proto)
minicloud_warnings(minicloud-worker)

add_executable(minicloud-gateway cpp/gateway/main.cpp cpp/gateway/proxy.cpp)
target_include_directories(minicloud-gateway PRIVATE cpp/include)
target_link_libraries(minicloud-gateway PRIVATE minicloud_common minicloud_valkey)
minicloud_warnings(minicloud-gateway)

add_executable(minicloudctl cpp/cli/main.cpp)
target_include_directories(minicloudctl PRIVATE cpp/include)
target_link_libraries(minicloudctl PRIVATE minicloud_common CURL::libcurl nlohmann_json::nlohmann_json)
minicloud_warnings(minicloudctl)

if(MINICLOUD_BUILD_TESTS)
  enable_testing()
  add_executable(minicloud-core-tests tests/cpp/core_tests.cpp)
  target_link_libraries(minicloud-core-tests PRIVATE minicloud_core)
  minicloud_warnings(minicloud-core-tests)
  add_test(NAME minicloud-core-tests COMMAND minicloud-core-tests)

  add_executable(minicloud-runtime-tests tests/cpp/runtime_tests.cpp)
  target_link_libraries(minicloud-runtime-tests PRIVATE minicloud_runtime)
  minicloud_warnings(minicloud-runtime-tests)
  add_test(NAME minicloud-runtime-tests COMMAND minicloud-runtime-tests)
endif()

install(TARGETS minicloud-controller minicloud-worker minicloud-gateway minicloudctl
        RUNTIME DESTINATION bin)
