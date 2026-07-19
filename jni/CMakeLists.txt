cmake_minimum_required(VERSION 3.29)
project(native-lib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLVM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/llvm")
set(CLANG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/clang")
set(LLVM_BUILD "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-project/build-android")

add_library(native SHARED native-lib.cpp)

target_include_directories(native PRIVATE
    "${LLVM_SRC}/include"
    "${LLVM_BUILD}/llvm/include"
    "${CLANG_SRC}/include"
    "${LLVM_BUILD}/clang/include"
)

target_compile_options(native PRIVATE -frtti -fexceptions)

find_library(log-lib log)

add_library(LLVM_LIB SHARED IMPORTED GLOBAL)
set_target_properties(LLVM_LIB PROPERTIES IMPORTED_LOCATION
    "${LIB_DIR}/libLLVM.so")

add_library(CLANG_LIB SHARED IMPORTED GLOBAL)
set_target_properties(CLANG_LIB PROPERTIES IMPORTED_LOCATION
    "${LIB_DIR}/libclang-cpp.so")

target_link_options(native PRIVATE -Wl,--allow-shlib-undefined)

target_link_libraries(native
    CLANG_LIB
    LLVM_LIB
    ${log-lib}
    android
    dl)
