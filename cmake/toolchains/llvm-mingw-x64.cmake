# Dev-only cross toolchain: build a Windows x86_64 DLL on Linux with LLVM-MinGW.
#
# Not a release path: the canonical Windows build is native clang-cl (MSVC ABI, x64-windows-static) on CI, see the clang-cl-x64-* presets. This
# produces a MinGW-ABI DLL as a fast Linux-side compile check of the Windows code paths. Expects LLVM-MinGW at /opt/llvm-mingw (override with
# LLVM_MINGW_ROOT).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED LLVM_MINGW_ROOT)
    set(LLVM_MINGW_ROOT "/opt/llvm-mingw")
endif()

set(TRIPLE_PREFIX "x86_64-w64-mingw32")

set(CMAKE_C_COMPILER "${LLVM_MINGW_ROOT}/bin/${TRIPLE_PREFIX}-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/${TRIPLE_PREFIX}-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW_ROOT}/bin/${TRIPLE_PREFIX}-windres")
set(CMAKE_AR "${LLVM_MINGW_ROOT}/bin/${TRIPLE_PREFIX}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${LLVM_MINGW_ROOT}/bin/${TRIPLE_PREFIX}-ranlib" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/${TRIPLE_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Self-contained DLL: statically link the C/C++ runtimes.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -static-libgcc")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static -static-libgcc")

# The net lane (apclientpp + OpenSSL/asio/zlib) builds via vcpkg's x64-mingw-static triplet, which uses this llvm-mingw driver through its
# x86_64-w64-mingw32-gcc wrapper.
