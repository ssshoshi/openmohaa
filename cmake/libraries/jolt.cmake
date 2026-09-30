# Jolt Physics, for the client-side props and ragdolls (cgame).
#
# Only the library is vendored (code/thirdparty/JoltPhysics/Jolt). Its own
# Build/CMakeLists.txt is not used: it rewrites the whole project's compiler
# flags and adds samples and tests. Jolt.cmake is included directly instead,
# with the options it reads set here; anything unset is off.
set(PHYSICS_REPO_ROOT ${SOURCE_DIR}/thirdparty/JoltPhysics)

# SSE 4.2 and no further, so no player's CPU is left out for an AVX build.
set(USE_SSE4_1 ON)
set(USE_SSE4_2 ON)
set(USE_AVX OFF)
set(USE_AVX2 OFF)
set(USE_AVX512 OFF)
set(USE_LZCNT OFF)
set(USE_TZCNT OFF)
set(USE_F16C OFF)
set(USE_FMADD OFF)

set(JPH_BUILD_SHARED_LIBS OFF)
set(OBJECT_LAYER_BITS 16)
set(ENABLE_OBJECT_STREAM OFF)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
set(JPH_USE_DX12 OFF)
set(JPH_USE_VK OFF)
set(JPH_USE_MTL OFF)
set(JPH_USE_CPU_COMPUTE OFF)

# Jolt.cmake sets the C++ standard, extensions and visibility for whatever is
# configured after it, not just for itself: left alone it turned the GNU
# extensions off for the whole build, which on MinGW drops the WIN32 macro and
# sends every #ifdef WIN32 in the engine down its Unix path. What it changes is
# put back afterwards; Jolt itself gets its settings as target properties.
set(_OPM_SAVED CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS CMAKE_CXX_VISIBILITY_PRESET)
foreach(_v ${_OPM_SAVED})
    if(DEFINED ${_v})
        set(_OPM_HAD_${_v} ON)
        set(_OPM_WAS_${_v} "${${_v}}")
    else()
        set(_OPM_HAD_${_v} OFF)
    endif()
endforeach()

# Jolt picks its SIMD flags from CMAKE_SYSTEM_PROCESSOR, which is the build
# host's when cross-compiling with a bare --target (as the Linux CI does): an
# i686 build on an arm64 runner got no -msse flags at all while the compiler
# still took Jolt's x86 intrinsics path. Hand it the architecture the compiler
# actually targets instead.
if(NOT MSVC)
    include(utils/arch)
    set(_OPM_WAS_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
    if(ARCH STREQUAL "x86" OR ARCH STREQUAL "i386")
        set(CMAKE_SYSTEM_PROCESSOR "x86")
    elseif(ARCH STREQUAL "arm64")
        set(CMAKE_SYSTEM_PROCESSOR "aarch64")
    else()
        set(CMAKE_SYSTEM_PROCESSOR "${ARCH}")
    endif()
endif()

include(${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake)

if(NOT MSVC)
    set(CMAKE_SYSTEM_PROCESSOR "${_OPM_WAS_PROCESSOR}")
endif()

foreach(_v ${_OPM_SAVED})
    if(_OPM_HAD_${_v})
        set(${_v} "${_OPM_WAS_${_v}}")
    else()
        unset(${_v})
    endif()
endforeach()
set_target_properties(Jolt PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Linked into a shared module.
set_property(TARGET Jolt PROPERTY POSITION_INDEPENDENT_CODE ON)
