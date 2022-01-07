cmake_minimum_required(VERSION 3.0)

project(xlog_decode)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -ggdb")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lz")

set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-party/)

# for zstd
option(ZSTD_BUILD_STATIC "BUILD STATIC LIBRARIES" ON)
option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" OFF)
set(ZSTD_SOURCE_DIR ${THIRD_PARTY_DIR}/zstd)
set(LIBRARY_DIR ${ZSTD_SOURCE_DIR}/lib)
include(GNUInstallDirs)
add_subdirectory(${ZSTD_SOURCE_DIR}/build/cmake/lib zstd)
include_directories(${THIRD_PARTY_DIR})

option(ARGPARSE_SHARED "Build shared library" OFF)
add_subdirectory(${THIRD_PARTY_DIR}/argparse)
add_subdirectory(${THIRD_PARTY_DIR}/cwalk)

set(XLOG_DECODE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_executable(xlog_decode 
    ${XLOG_DECODE_SRCS}/decode_log_file.c 
    ${XLOG_DECODE_SRCS}/micro-ecc-master/uECC.c)

target_link_libraries(xlog_decode
    libzstd_static
    argparse
    cwalk)
