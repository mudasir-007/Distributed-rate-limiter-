cmake_minimum_required(VERSION 3.25)

project(distributed_rate_limiter VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(USE_TSAN "Build with ThreadSanitizer (Clang/GCC)" OFF)
option(BUILD_BENCHMARKS "Build benchmark executables" ON)

find_package(Threads REQUIRED)

# Dependencies are intentionally resolved through normal CMake discovery so
# the project works with vcpkg, system packages, or a compatible package
# manager. vcpkg.json provides the canonical dependency list.
find_path(REDIS_PLUS_PLUS_INCLUDE_DIR NAMES sw/redis++/redis++.h)
find_library(REDIS_PLUS_PLUS_LIB NAMES redis++)
find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h)
find_library(HIREDIS_LIB NAMES hiredis)

if(NOT REDIS_PLUS_PLUS_INCLUDE_DIR OR NOT REDIS_PLUS_PLUS_LIB
   OR NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
  message(FATAL_ERROR
    "redis-plus-plus and hiredis are required. "
    "Use the setup script or configure with the vcpkg toolchain. "
    "See README.md for platform-specific instructions.")
endif()

# CMake-built binaries default to the copied script directory when launched
# from the build tree. Tests receive the source-tree path explicitly.
set(RATE_LIMITER_SCRIPT_DIR "${CMAKE_SOURCE_DIR}/scripts" CACHE PATH
    "Directory containing token_bucket.lua and leader_renew.lua")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts")
configure_file(
  "${PROJECT_SOURCE_DIR}/scripts/token_bucket.lua"
  "${CMAKE_BINARY_DIR}/scripts/token_bucket.lua"
  COPYONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/scripts/leader_renew.lua"
  "${CMAKE_BINARY_DIR}/scripts/leader_renew.lua"
  COPYONLY
)

add_library(ratelimiter
  src/token_bucket.cpp
  src/thread_safe_token_bucket.cpp
  src/sharded_rate_limiter.cpp
  src/redis_backend.cpp
  src/leader_elector.cpp
)

target_include_directories(ratelimiter
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${REDIS_PLUS_PLUS_INCLUDE_DIR}
    ${HIREDIS_INCLUDE_DIR}
)

target_link_libraries(ratelimiter
  PUBLIC
    Threads::Threads
    ${REDIS_PLUS_PLUS_LIB}
    ${HIREDIS_LIB}
)

if(WIN32)
  target_link_libraries(ratelimiter PUBLIC ws2_32)
endif()

if(USE_TSAN)
  if(MSVC)
    message(FATAL_ERROR "USE_TSAN is not supported with MSVC. Use Clang or GCC.")
  endif()
  target_compile_options(ratelimiter PUBLIC
    -fsanitize=thread -fno-omit-frame-pointer
  )
  target_link_options(ratelimiter PUBLIC -fsanitize=thread)
endif()

add_executable(server src/server_main.cpp)
target_link_libraries(server PRIVATE ratelimiter)

add_executable(concurrency_demo src/concurrency_demo_main.cpp)
target_link_libraries(concurrency_demo PRIVATE ratelimiter)

if(BUILD_BENCHMARKS)
  add_executable(bench src/bench_main.cpp)
  target_link_libraries(bench PRIVATE ratelimiter)

  add_executable(bench_redis src/bench_redis_main.cpp)
  target_link_libraries(bench_redis PRIVATE ratelimiter)

  add_executable(bench_contention src/bench_contention_main.cpp)
  target_link_libraries(bench_contention PRIVATE Threads::Threads)
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "Distributed Rate Limiter ${PROJECT_VERSION}")
message(STATUS "  C++ standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Script directory: ${RATE_LIMITER_SCRIPT_DIR}")
message(STATUS "  Benchmarks: ${BUILD_BENCHMARKS}")
message(STATUS "  ThreadSanitizer: ${USE_TSAN}")
message(STATUS "")
