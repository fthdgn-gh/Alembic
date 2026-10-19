cmake_minimum_required(VERSION 3.20)
project(Alembic VERSION 0.1.0 LANGUAGES C CXX)

option(ALEMBIC_BUILD_CLI "Build the alembic command-line tool" ON)
option(ALEMBIC_BUILD_TESTS "Build the unit tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# zlib: the system's on a PC; the PS5 payload SDK has none, so the bundled
# zlib 1.3.1 (third_party/zlib, official release sources) is built there.
if(PS5)
    set(ALEMBIC_BUNDLED_ZLIB_DEFAULT ON)
else()
    set(ALEMBIC_BUNDLED_ZLIB_DEFAULT OFF)
endif()
option(ALEMBIC_BUNDLED_ZLIB "Build the bundled zlib instead of using the system's" ${ALEMBIC_BUNDLED_ZLIB_DEFAULT})
if(ALEMBIC_BUNDLED_ZLIB)
    add_library(alembic_zlib STATIC
        third_party/zlib/adler32.c third_party/zlib/compress.c third_party/zlib/crc32.c
        third_party/zlib/deflate.c third_party/zlib/infback.c third_party/zlib/inffast.c
        third_party/zlib/inflate.c third_party/zlib/inftrees.c third_party/zlib/trees.c
        third_party/zlib/uncompr.c third_party/zlib/zutil.c)
    target_include_directories(alembic_zlib PUBLIC third_party/zlib)
    target_compile_options(alembic_zlib PRIVATE -O3)
    if(NOT WIN32)
        target_compile_definitions(alembic_zlib PRIVATE HAVE_UNISTD_H)
    endif()
    add_library(ZLIB::ZLIB ALIAS alembic_zlib)
else()
    find_package(ZLIB REQUIRED)
endif()
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
    src/core/ampr_emu.cpp
    src/core/convert.cpp
    src/core/crypto.cpp
    src/core/exfat_image.cpp
    src/core/exfat_reader.cpp
    src/core/ffpfsc.cpp
    src/core/fpkg.cpp
    src/core/fpkg_inner.cpp
    src/core/io.cpp
    src/core/resume.cpp
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
    src/core/ufs2_image.cpp
    src/core/pfs_image.cpp
    src/core/fpkg_inner_build.cpp
    src/core/fpkg_media.cpp
    src/core/fpkg_naps_build.cpp
    src/core/fpkg_outer_build.cpp
    src/core/fpkg_keys.cpp
    src/core/fpkg_playgo_build.cpp
    src/core/fpkg_cnt_build.cpp
    src/core/fpkg_package_build.cpp
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

# The engine: job queue + HTTP API + web page. alembic.elf on the PS5 (a
# payload), alembic-engine on a PC for testing.
option(ALEMBIC_BUILD_ENGINE "Build the conversion engine" ON)
if(ALEMBIC_BUILD_ENGINE)
    set(ALEMBIC_WEB_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/engine_index_html.cpp)
    add_custom_command(
        OUTPUT ${ALEMBIC_WEB_SOURCE}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/engine/web/index.html
                -DOUTPUT=${ALEMBIC_WEB_SOURCE} -DSYMBOL=kIndexHtml -DNAMESPACE=alembic::engine
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        COMMENT "Embedding the engine web page"
        VERBATIM)
    set(ALEMBIC_I18N_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/engine_i18n_js.cpp)
    add_custom_command(
        OUTPUT ${ALEMBIC_I18N_SOURCE}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/engine/web/i18n.js
                -DOUTPUT=${ALEMBIC_I18N_SOURCE} -DSYMBOL=kI18nJs -DNAMESPACE=alembic::engine
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/web/i18n.js ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        COMMENT "Embedding the page translations"
        VERBATIM)
    set(ALEMBIC_ICON_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/engine_icon_svg.cpp)
    add_custom_command(
        OUTPUT ${ALEMBIC_ICON_SOURCE}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/alembic.svg
                -DOUTPUT=${ALEMBIC_ICON_SOURCE} -DSYMBOL=kIconSvg -DNAMESPACE=alembic::engine
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/alembic.svg ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        COMMENT "Embedding the icon"
        VERBATIM)
    set(ALEMBIC_ICON0_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/engine_icon0_png.cpp)
    add_custom_command(
        OUTPUT ${ALEMBIC_ICON0_SOURCE}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon0.png
                -DOUTPUT=${ALEMBIC_ICON0_SOURCE} -DSYMBOL=kIcon0Png -DNAMESPACE=alembic::engine
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/icon/icon0.png ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
        COMMENT "Embedding the home-screen icon"
        VERBATIM)
    if(PS5)
        set(ALEMBIC_ENGINE_PLATFORM src/engine/platform_ps5.cpp)
    else()
        set(ALEMBIC_ENGINE_PLATFORM src/engine/platform_host.cpp)
    endif()
    add_library(alembic_engine_lib STATIC
        src/engine/deps.cpp
        src/engine/fs_ops.cpp
        src/engine/ftp.cpp
        src/engine/game_meta.cpp
        src/engine/http.cpp
        src/engine/jobs.cpp
        src/engine/json_value.cpp
        src/engine/l10n.cpp
        src/engine/remote.cpp
        src/engine/runner.cpp
        src/engine/smp.cpp
        ${ALEMBIC_ENGINE_PLATFORM}
    )
    target_include_directories(alembic_engine_lib PUBLIC src/engine)
    target_compile_options(alembic_engine_lib PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    target_link_libraries(alembic_engine_lib PUBLIC alembic_core)
    add_executable(alembic_engine src/engine/main.cpp ${ALEMBIC_WEB_SOURCE} ${ALEMBIC_I18N_SOURCE} ${ALEMBIC_ICON_SOURCE} ${ALEMBIC_ICON0_SOURCE})
    target_compile_options(alembic_engine PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    target_compile_definitions(alembic_engine PRIVATE ALEMBIC_VERSION="${PROJECT_VERSION}")
    target_link_libraries(alembic_engine PRIVATE alembic_engine_lib)
    if(PS5)
        set_target_properties(alembic_engine PROPERTIES OUTPUT_NAME alembic.elf)
        target_link_libraries(alembic_engine PRIVATE SceNotification SceSystemService SceUserService SceAppInstUtil SceNet SceSsl SceHttp)
    else()
        set_target_properties(alembic_engine PROPERTIES OUTPUT_NAME alembic-engine)
    endif()
endif()

if(ALEMBIC_BUILD_TESTS)
    enable_testing()
    add_executable(alembic_tests
        tests/test_main.cpp
        tests/test_ampr.cpp
        tests/test_ampr_emu.cpp
        tests/test_convert.cpp
        tests/test_crypto.cpp
        tests/test_exfat.cpp
        tests/test_ffpfsc.cpp
        tests/test_fpkg_build.cpp
        tests/test_json.cpp
        tests/test_lz4pack.cpp
        tests/test_pfs.cpp
        tests/test_pfsc.cpp
        tests/test_resume.cpp
        tests/test_sha256.cpp
        tests/test_toml.cpp
        tests/test_unicode.cpp
    )
    target_compile_options(alembic_tests PRIVATE ${ALEMBIC_COMPILE_OPTIONS})
    # Tests that need reference files from .refs/ skip themselves when absent.
    target_compile_definitions(alembic_tests PRIVATE ALEMBIC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(ALEMBIC_BUILD_ENGINE)
        target_sources(alembic_tests PRIVATE tests/test_engine.cpp)
        target_link_libraries(alembic_tests PRIVATE alembic_engine_lib) # includes alembic_core
    else()
        target_link_libraries(alembic_tests PRIVATE alembic_core)
    endif()
    add_test(NAME alembic_tests COMMAND alembic_tests)
endif()
