include_guard(GLOBAL)

set(RENDERER_COMMON_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_font.c
    ${SOURCE_DIR}/renderercommon/tr_image_bmp.c
    ${SOURCE_DIR}/renderercommon/tr_image_jpg.c
    ${SOURCE_DIR}/renderercommon/tr_image_pcx.c
    ${SOURCE_DIR}/renderercommon/tr_image_png.c
    ${SOURCE_DIR}/renderercommon/tr_image_pvr.c
    ${SOURCE_DIR}/renderercommon/tr_image_tga.c
    ${SOURCE_DIR}/renderercommon/tr_noise.c
    ${SOURCE_DIR}/renderercommon/puff.c
	${SOURCE_DIR}/tiki/tiki_mesh.cpp
)

set(SDL_RENDERER_SOURCES
    ${SOURCE_DIR}/sdl/sdl_gamma.c
    ${SOURCE_DIR}/sdl/sdl_glimp.c
)

set(DYNAMIC_RENDERER_SOURCES
    ${SOURCE_DIR}/renderercommon/tr_subs.c
    ${SOURCE_DIR}/qcommon/q_shared.c
    ${SOURCE_DIR}/qcommon/q_math.c
    ${SOURCE_DIR}/corepp/str.cpp
)

if(USE_FREETYPE)
    list(APPEND RENDERER_DEFINITIONS BUILD_FREETYPE)
endif()

if(USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS USE_RENDERER_DLOPEN)
    list(APPEND RENDERER_DEFINITIONS REF_DLL=1)
elseif(BUILD_RENDERER_GL1 AND BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Multiple static renderers enabled; choose one")
elseif(NOT BUILD_RENDERER_GL1 AND NOT BUILD_RENDERER_GL2)
    message(FATAL_ERROR "Zero static renderers enabled; choose one")
endif()

# A renderer is dlopen()ed at runtime, so an unresolved symbol would otherwise
# not surface until load time -- where the client quietly falls back to the
# other renderer and the problem looks like "my changes had no effect".
if(USE_RENDERER_DLOPEN AND NOT APPLE AND NOT MSVC)
    include(CheckLinkerFlag OPTIONAL RESULT_VARIABLE HAVE_CHECK_LINKER_FLAG)
    if(HAVE_CHECK_LINKER_FLAG)
        check_linker_flag(C "-Wl,--no-undefined" RENDERER_HAS_NO_UNDEFINED)
        if(RENDERER_HAS_NO_UNDEFINED)
            list(APPEND RENDERER_LINK_OPTIONS "-Wl,--no-undefined")
        endif()
    endif()
endif()

list(APPEND RENDERER_LIBRARIES ${COMMON_LIBRARIES})

