# Cross-compile to Windows ARM64 with llvm-mingw (clang + lld).
# Debian's gcc mingw-w64 only targets x86, so ARM Windows uses llvm-mingw.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_PREFIX aarch64-w64-mingw32)
set(LLVM_MINGW /opt/llvm-mingw)
set(CMAKE_C_COMPILER   ${LLVM_MINGW}/bin/${TOOLCHAIN_PREFIX}-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/${TOOLCHAIN_PREFIX}-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW}/bin/${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR           ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB       ${LLVM_MINGW}/bin/llvm-ranlib)

set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
