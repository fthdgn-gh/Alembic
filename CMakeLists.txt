cmake_minimum_required(VERSION 3.20)
project(Alembic VERSION 0.1.0 LANGUAGES CXX)

option(ALEMBIC_BUILD_CLI "Build the alembic command-line tool" ON)
option(ALEMBIC_BUILD_TESTS "Build the unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)

# The PS5 build forbids exceptions and RTTI, so the host build does too.
set(ALEMBIC_COMPILE_OPTIONS -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)

add_library(alembic_core STATIC
    src/core/ampr.cpp
    src/core/convert.cpp
    src/core/crypto.cpp
    src/core/exfat_image.cpp
    src/core/exfat_reader.cpp
    src/core/ffpfsc.cpp
    src/core/fpkg.cpp
    src/core/io.cpp
    src/core/json.cpp
    src/core/pfsc.cpp
    src/core/sha256.cpp
    src/core/tree.cpp
    src/core/unicode.cpp
)
target_include_directories(alembic_core
    PUBLIC src/core/include
    PRIVATE src/core
)
target_compile_options(alembic_core PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
target_link_libraries(alembic_core PUBLIC ZLIB::ZLIB)

if(ALEMBIC_BUILD_CLI)
    add_executable(alembic src/cli/main.cpp)
    target_compile_options(alembic PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    target_link_libraries(alembic PRIVATE alembic_core)
endif()

if(ALEMBIC_BUILD_TESTS)
    enable_testing()
    add_executable(alembic_tests
        tests/test_main.cpp
        tests/test_ampr.cpp
        tests/test_convert.cpp
        tests/test_crypto.cpp
        tests/test_exfat.cpp
        tests/test_ffpfsc.cpp
        tests/test_json.cpp
        tests/test_pfsc.cpp
        tests/test_sha256.cpp
        tests/test_unicode.cpp
    )
    target_compile_options(alembic_tests PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    target_link_libraries(alembic_tests PRIVATE alembic_core)
    add_test(NAME alembic_tests COMMAND alembic_tests)
endif()
