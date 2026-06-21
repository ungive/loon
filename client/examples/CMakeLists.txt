set(SOURCES client.cpp)

# cmake scripts
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake)

# select the loon backend here
set(LOON_WEBSOCKET_BACKEND "LIBHV" CACHE BOOL "websocket backend" FORCE)

add_executable(loon-cli ${SOURCES})
target_link_libraries(loon-cli loon)
get_filename_component(PARENT_DIR ../ ABSOLUTE)
target_include_directories(loon-cli PRIVATE ${PARENT_DIR}/include)

# qt dependency, if it's used
if(LOON_WEBSOCKET_BACKEND STREQUAL "QT")
    find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)
    target_link_libraries(loon-cli Qt6::Core Qt6::Network Qt6::WebSockets)
    target_compile_definitions(loon-cli PRIVATE USES_QT)

    if (WIN32)
        include(windeployqt)
        windeployqt(loon-cli)
        add_custom_command(
            TARGET loon-cli
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_directory_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/windeployqt/
                $<TARGET_FILE_DIR:loon-cli>)
    endif()
endif()

# tls backend for loon
find_package(OpenSSL REQUIRED)
target_link_libraries(loon-cli OpenSSL::SSL OpenSSL::Crypto)

set_property(TARGET loon-cli PROPERTY CXX_STANDARD 17)
set_property(TARGET loon-cli PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32 AND DEFINED CONAN_RUNTIME_LIB_DIRS)
    foreach(CONAN_RUNTIME_LIB_DIR ${CONAN_RUNTIME_LIB_DIRS})
        add_custom_command(
            TARGET loon-cli
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -DSRC_DIR=${CONAN_RUNTIME_LIB_DIR}
                -DDEST_DIR=$<TARGET_FILE_DIR:loon-cli> -DQUIET=1 -P
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/copy_dlls.cmake"
            VERBATIM)
    endforeach()
endif()
