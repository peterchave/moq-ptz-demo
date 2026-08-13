cmake_minimum_required(VERSION 3.16)
project(moq2ptz C)

# ── moq5 paths ────────────────────────────────────────────────────────
#
# All moq5 / picoquic / picotls libraries are pre-built in MOQ5_BUILD.
# We create IMPORTED targets directly — no FindPicoquic/FindPTLS run,
# no recompilation of any dependency.
#
# Override on the cmake command line if your layout differs:
#   cmake -B build -DMOQ5_ROOT=/path/to/moq5
#
set(MOQ5_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/../moq5"
    CACHE PATH "moq5 repository root")
set(MOQ5_BUILD "${MOQ5_ROOT}/build"
    CACHE PATH "moq5 CMake build directory")
set(MOQ5_PTLS_BUILD "${MOQ5_ROOT}/thirdparty/picotls/build"
    CACHE PATH "picotls build directory (adjacent to moq5/thirdparty/picoquic)")

# ── System dependencies ───────────────────────────────────────────────

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# ── Pre-built IMPORTED targets ────────────────────────────────────────

# picotls
add_library(ptls::core       STATIC IMPORTED)
add_library(ptls::openssl    STATIC IMPORTED)
add_library(ptls::minicrypto STATIC IMPORTED)
set_target_properties(ptls::core PROPERTIES
    IMPORTED_LOCATION "${MOQ5_PTLS_BUILD}/libpicotls-core.a")
set_target_properties(ptls::openssl PROPERTIES
    IMPORTED_LOCATION "${MOQ5_PTLS_BUILD}/libpicotls-openssl.a"
    INTERFACE_LINK_LIBRARIES "ptls::core;OpenSSL::SSL;OpenSSL::Crypto")
set_target_properties(ptls::minicrypto PROPERTIES
    IMPORTED_LOCATION "${MOQ5_PTLS_BUILD}/libpicotls-minicrypto.a"
    INTERFACE_LINK_LIBRARIES "ptls::core")

# picoquic
add_library(picoquic::log  STATIC IMPORTED)
add_library(picoquic::core STATIC IMPORTED)
set_target_properties(picoquic::log PROPERTIES
    IMPORTED_LOCATION "${MOQ5_BUILD}/_deps/picoquic/libpicoquic-log.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MOQ5_ROOT}/thirdparty/picoquic/picoquic")
set_target_properties(picoquic::core PROPERTIES
    IMPORTED_LOCATION "${MOQ5_BUILD}/_deps/picoquic/libpicoquic-core.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MOQ5_ROOT}/thirdparty/picoquic/picoquic"
    INTERFACE_LINK_LIBRARIES
        "picoquic::log;ptls::openssl;ptls::minicrypto;ptls::core;OpenSSL::SSL;OpenSSL::Crypto")

# moq core
add_library(moq::core STATIC IMPORTED)
set_target_properties(moq::core PROPERTIES
    IMPORTED_LOCATION "${MOQ5_BUILD}/core/libmoq-core.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MOQ5_ROOT}/core/include"
    INTERFACE_COMPILE_FEATURES "c_std_11")

# moq adapter-picoquic
add_library(moq::adapter-picoquic STATIC IMPORTED)
set_target_properties(moq::adapter-picoquic PROPERTIES
    IMPORTED_LOCATION
        "${MOQ5_BUILD}/adapters/picoquic/libmoq-adapter-picoquic.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${MOQ5_ROOT}/adapters/picoquic/include;${MOQ5_ROOT}/adapters/picoquic"
    INTERFACE_LINK_LIBRARIES
        "moq::core;OpenSSL::SSL;OpenSSL::Crypto;picoquic::core;picoquic::log")

# moq adapter-picoquic-threaded
add_library(moq::adapter-picoquic-threaded STATIC IMPORTED)
set_target_properties(moq::adapter-picoquic-threaded PROPERTIES
    IMPORTED_LOCATION
        "${MOQ5_BUILD}/adapters/picoquic/libmoq-adapter-picoquic-threaded.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${MOQ5_ROOT}/adapters/picoquic/include"
    INTERFACE_LINK_LIBRARIES
        "moq::adapter-picoquic;Threads::Threads")

# ── Executable ────────────────────────────────────────────────────────

add_executable(ptz_control ptz_control.c)
target_link_libraries(ptz_control PRIVATE moq::adapter-picoquic-threaded)
set_target_properties(ptz_control PROPERTIES C_STANDARD 11)
