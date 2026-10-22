# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-builds the Windows executables with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw). LLVM_MINGW points at its folder;
# without it, the x86_64-w64-mingw32-* compilers are looked up on the PATH.
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW AND DEFINED ENV{LLVM_MINGW})
    set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
set(LLVM_MINGW "${LLVM_MINGW}" CACHE PATH "llvm-mingw folder" FORCE)
set(_prefix x86_64-w64-mingw32-)
if(LLVM_MINGW)
    set(_prefix ${LLVM_MINGW}/bin/x86_64-w64-mingw32-)
    set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
endif()
set(CMAKE_C_COMPILER ${_prefix}clang)
set(CMAKE_CXX_COMPILER ${_prefix}clang++)
set(CMAKE_RC_COMPILER ${_prefix}windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
