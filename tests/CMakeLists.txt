include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)

enable_testing()

# CTest executes GoogleTest cases from the build tree. Inject the repository's
# Lua directory automatically so tests do not depend on the caller's cwd.
set(RATE_LIMITER_SCRIPT_DIR "${CMAKE_SOURCE_DIR}/scripts")

file(GLOB UNIT_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

add_executable(unit_tests ${UNIT_TESTS})

target_link_libraries(unit_tests PRIVATE
    ratelimiter
    GTest::gtest_main
)

include(GoogleTest)

gtest_discover_tests(
    unit_tests
    PROPERTIES
        ENVIRONMENT "RATE_LIMITER_SCRIPT_DIR=${RATE_LIMITER_SCRIPT_DIR}"
)

# Standalone stress/smoke executables are built for direct use. They are not
# added to CTest because some are intentionally long-running or require Redis.
file(GLOB MANUAL_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/manual_*.cpp"
)

foreach(TEST_FILE ${MANUAL_TESTS})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_FILE})
    target_link_libraries(${TEST_NAME} PRIVATE ratelimiter)
endforeach()

file(GLOB SMOKE_TESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*smoke_test.cpp"
)

foreach(TEST_FILE ${SMOKE_TESTS})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_FILE})
    target_link_libraries(${TEST_NAME} PRIVATE ratelimiter)
endforeach()
