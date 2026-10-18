cmake_minimum_required(VERSION 3.20)
project(Alembic VERSION 0.1.0 LANGUAGES C CXX)

option(ALEMBIC_BUILD_CLI "Build the alembic command-line tool" ON)
option(ALEMBIC_BUILD_TESTS "Build the unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# The PS5 build forbids exceptions and RTTI, so the host build does too.
set(ALEMBIC_COMPILE_OPTIONS -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)

# LZ4 1.10.0 (BSD-2-Clause), the version ampr_emu's runtime pins.
add_library(alembic_lz4 STATIC third_party/lz4/lz4.c third_party/lz4/lz4hc.c third_party/lz4/xxhash.c)
target_include_directories(alembic_lz4 PUBLIC third_party/lz4)
target_compile_options(alembic_lz4 PRIVATE -O3)

# The console-tested LZ4 pack profiles in profiles/lz4/, built into the binary.
set(ALEMBIC_PROFILE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/profiles/lz4)
file(GLOB ALEMBIC_PROFILE_FILES CONFIGURE_DEPENDS ${ALEMBIC_PROFILE_DIR}/*.toml)
set(ALEMBIC_PROFILE_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/lz4_profiles_data.cpp)
# Copied files can be older than the generated source, so a changed file list
# must regenerate it too.
set(ALEMBIC_PROFILE_LIST ${CMAKE_CURRENT_BINARY_DIR}/lz4_profiles.list)
string(REPLACE ";" "\n" ALEMBIC_PROFILE_LIST_TEXT "${ALEMBIC_PROFILE_FILES}")
file(CONFIGURE OUTPUT ${ALEMBIC_PROFILE_LIST} CONTENT "${ALEMBIC_PROFILE_LIST_TEXT}\n")
add_custom_command(
    OUTPUT ${ALEMBIC_PROFILE_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_DIR=${ALEMBIC_PROFILE_DIR} -DOUTPUT=${ALEMBIC_PROFILE_SOURCE}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    DEPENDS ${ALEMBIC_PROFILE_FILES} ${ALEMBIC_PROFILE_LIST} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_files.cmake
    COMMENT "Embedding LZ4 pack profiles"
    VERBATIM)

add_library(alembic_core STATIC
    ${ALEMBIC_PROFILE_SOURCE}
    src/core/ampr.cpp
    src/core/convert.cpp
    src/core/crypto.cpp
    src/core/exfat_image.cpp
    src/core/exfat_reader.cpp
    src/core/ffpfsc.cpp
    src/core/fpkg.cpp
    src/core/fpkg_inner.cpp
    src/core/io.cpp
    src/core/json.cpp
    src/core/kraken.cpp
    src/core/lz4pack_build.cpp
    src/core/lz4pack_format.cpp
    src/core/lz4pack_profile.cpp
    src/core/lz4pack_read.cpp
    src/core/pfsc.cpp
    src/core/sha256.cpp
    src/core/toml.cpp
    src/core/tree.cpp
    src/core/ufs2.cpp
    src/core/unicode.cpp
)
target_include_directories(alembic_core
    PUBLIC src/core/include
    PRIVATE src/core
)
target_compile_options(alembic_core PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
target_link_libraries(alembic_core PUBLIC ZLIB::ZLIB Threads::Threads PRIVATE alembic_lz4)

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
        tests/test_lz4pack.cpp
        tests/test_pfsc.cpp
        tests/test_sha256.cpp
        tests/test_toml.cpp
        tests/test_unicode.cpp
    )
    target_compile_options(alembic_tests PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    target_link_libraries(alembic_tests PRIVATE alembic_core)
    add_test(NAME alembic_tests COMMAND alembic_tests)
endif()
