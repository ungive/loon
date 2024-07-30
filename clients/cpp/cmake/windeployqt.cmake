# https://stackoverflow.com/a/60856725/6748004

find_package(Qt6Core REQUIRED)

get_target_property(_qmake_executable Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)

function(windeployqt target)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND
            "${_qt_bin_dir}/windeployqt.exe" --verbose 1
            "$<IF:$<CONFIG:Debug>,--debug,--release>" --no-svg --no-opengl
            --no-opengl-sw --no-compiler-runtime --no-system-d3d-compiler --dir
            "${CMAKE_CURRENT_BINARY_DIR}/windeployqt"
            \"$<TARGET_FILE:${target}>\"
        COMMENT
            "Deploying Qt libraries using windeployqt for compilation target '${target}' ..."
    )
endfunction()
