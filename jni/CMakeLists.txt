cmake_minimum_required(VERSION 3.18)
project(native-lib CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLVM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/llvm")
set(CLANG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/clang")
set(LLVM_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/build-android")

add_library(native-lib SHARED native-lib.cpp)

target_include_directories(native-lib PRIVATE
    "${LLVM_SRC}/include"
    "${CLANG_SRC}/include"
    "${LLVM_BUILD}/llvm/include"
    "${LLVM_BUILD}/clang/include"
)

target_compile_options(native-lib PRIVATE -frtti -fexceptions)

find_library(LLVM_LIB LLVM PATHS "${LIB_DIR}" NO_DEFAULT_PATH)
find_library(CLANG_LIB clang-cpp PATHS "${LIB_DIR}" NO_DEFAULT_PATH)

if(NOT LLVM_LIB OR NOT CLANG_LIB)
    message(FATAL_ERROR "Libraries not found. LIB_DIR=${LIB_DIR}")
endif()

target_link_libraries(native-lib
    ${CLANG_LIB}
    ${LLVM_LIB}
    android
    log
    dl
)
