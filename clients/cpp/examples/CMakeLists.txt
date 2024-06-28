set(SOURCES client.cpp)

add_executable(loon-cli ${SOURCES})
target_link_libraries(loon-cli loon)
get_filename_component(PARENT_DIR ../ ABSOLUTE)
target_include_directories(loon-cli PRIVATE ${PARENT_DIR}/include)

set_property(TARGET loon-cli PROPERTY CXX_STANDARD 17)
set_property(TARGET loon-cli PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

add_custom_command(
    TARGET loon-cli
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:loon-cli>
            $<TARGET_FILE_DIR:loon-cli>
    COMMAND_EXPAND_LISTS)
