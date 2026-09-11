# Cross compile Windows x86_64 binaries from a Unix host using mingw-w64.
#
#   cmake -S . -B build/win64 \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#       -DBUILD_RENDERER_GL2=ON -DUSE_RENDERER_DLOPEN=ON
#
# The Windows SDL2 import libraries and DLL are already in the tree under
# code/thirdparty/libs/win64, so USE_INTERNAL_SDL (on by default) is enough and
# no Windows SDK or prebuilt dependency tree is needed.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TARGET x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${MINGW_TARGET}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_TARGET}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_TARGET}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TARGET})

# Link the GCC and C++ runtimes in, so the binaries do not need
# libgcc_s_seh-1.dll and libstdc++-6.dll sitting next to them. This matches how
# the shipped Windows binaries behave -- they import only SDL2, KERNEL32 and
# msvcrt -- and keeps a renderer DLL a drop-in file on its own.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Look for headers and libraries in the target tree, but run build tools such as
# flex and bison from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
