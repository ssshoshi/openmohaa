# The fork's own game data: every folder under data/ is zipped into a pk3 of
# the same name, zzzzzzzzz- prefixed so it loads after the stock and mod paks.
# They land in <build>/data/ and are installed to main/ beside the stock paks.
#
#   data/opm-bugreport/ui/bugreport.urc -> zzzzzzzzz-opm-bugreport.pk3

# CONFIGURE_DEPENDS: a new folder under data/ is picked up by the next build,
# not only by the next cmake run.
file(GLOB _OPM_DATA_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/data/*)

set(_OPM_PAKS)
foreach(_dir ${_OPM_DATA_DIRS})
    if(NOT IS_DIRECTORY ${_dir})
        continue()
    endif()

    get_filename_component(_name ${_dir} NAME)
    set(_pak ${CMAKE_BINARY_DIR}/data/zzzzzzzzz-${_name}.pk3)
    # Relative for the archive's paths, absolute for the dependencies.
    file(GLOB_RECURSE _files CONFIGURE_DEPENDS RELATIVE ${_dir} ${_dir}/*)
    list(TRANSFORM _files PREPEND ${_dir}/ OUTPUT_VARIABLE _deps)

    add_custom_command(
        OUTPUT ${_pak}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/data
        COMMAND ${CMAKE_COMMAND} -E rm -f ${_pak}
        COMMAND ${CMAKE_COMMAND} -E tar cf ${_pak} --format=zip -- ${_files}
        WORKING_DIRECTORY ${_dir}
        DEPENDS ${_deps}
        COMMENT "Packing data/${_name} into zzzzzzzzz-${_name}.pk3"
        VERBATIM
    )
    list(APPEND _OPM_PAKS ${_pak})
endforeach()

if(_OPM_PAKS)
    add_custom_target(opm_data ALL DEPENDS ${_OPM_PAKS})
    install(FILES ${_OPM_PAKS} DESTINATION ${INSTALL_BINDIR_FULL}/main)
endif()
