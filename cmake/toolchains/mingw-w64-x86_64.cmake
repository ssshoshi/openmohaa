# Cross-compile OpenMoHAA for 64-bit Windows from Linux using mingw-w64.
#
#   cmake -S . -B .cmake-win -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<this file> \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo
#
# Flex and Bison deliberately resolve to the host binaries: they generate C
# source at build time and must run on the build machine, not the target.
# OpenAL and cURL are both loaded at runtime and only need their headers, which
# the repository bundles, so no Windows import libraries are required for them.
# SDL2 links against the MinGW import libraries already in
# code/thirdparty/libs/win64.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

# The -posix variants are required: the default win32 thread model has no
# C++11 threading, and code/sys/sys_update_checker.cpp uses std::thread,
# std::mutex and std::condition_variable.
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc-posix)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++-posix)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_AR           ${TOOLCHAIN_PREFIX}-ar)
set(CMAKE_RANLIB       ${TOOLCHAIN_PREFIX}-ranlib)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Programs come from the host (flex, bison); everything else from the target
# sysroot, so a stray Linux libcurl or libopenal cannot be picked up.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Keep the runtime self-contained so the binaries drop straight into an
# existing install without needing libgcc/libstdc++ DLLs alongside them.
# -static also pulls in libwinpthread, which the posix thread model would
# otherwise require as a separate DLL next to the binaries.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static")
