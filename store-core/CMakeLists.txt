cmake_minimum_required(VERSION 3.14)
project(FieldStorePlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found")
endif()

# LEZ v0.3.1 dropped the built-in Token program, so Field Store deploys its own
# (../token-program) and proves private payments against that binary. Embed it
# as a byte array; src/token.bin is a copy of ../token-program/token.bin.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/token.bin" TOKEN_ELF_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," TOKEN_ELF_BYTES "${TOKEN_ELF_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/token_elf.inc" "${TOKEN_ELF_BYTES}\n")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

# The LEZ wallet FFI (libwallet_ffi) does the heavy lifting: key storage,
# account views, fee handling and — for private payments — running the Token
# program locally and proving it. It is built from the pinned LEZ flake input
# and bundled next to the plugin.
logos_module(
    NAME fieldstore_core
    SOURCES
        src/token_codec.h
        src/fieldstore_impl.h
        src/fieldstore_impl.cpp
    EXTERNAL_LIBS
        wallet_ffi
)
