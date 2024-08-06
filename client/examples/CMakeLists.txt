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

# tls backend for loon
find_package(OpenSSL REQUIRED)
target_link_libraries(loon-cli OpenSSL::SSL OpenSSL::Crypto)

set_property(TARGET loon-cli PROPERTY CXX_STANDARD 17)
set_property(TARGET loon-cli PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

add_custom_command(
    TARGET loon-cli
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:loon-cli> $<TARGET_FILE_DIR:loon-cli>
    COMMAND_EXPAND_LISTS)

# Copy DLLs that aren't included with TARGET_RUNTIME_DLLS for some reason.
set(MISSING_DLLS "")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND MISSING_DLLS zlibd1.dll)
    list(APPEND MISSING_DLLS debug/bin/libcrypto-3-x64.dll)
    list(APPEND MISSING_DLLS debug/bin/libssl-3-x64.dll)
    list(APPEND MISSING_DLLS debug/bin/legacy.dll)
else()
    list(APPEND MISSING_DLLS zlib1.dll)
    list(APPEND MISSING_DLLS libcrypto-3-x64.dll)
    list(APPEND MISSING_DLLS libssl-3-x64.dll)
    list(APPEND MISSING_DLLS legacy.dll)
endif()
foreach(MISSING_DLL ${MISSING_DLLS})
    foreach(PREFIX_PATH ${CMAKE_PREFIX_PATH})
        file(GLOB_RECURSE MISSING_DLL_PATHS ${PREFIX_PATH}/${MISSING_DLL})
        file(GLOB_RECURSE MISSING_DLL_PATHS_NESTED
             ${PREFIX_PATH}/**/${MISSING_DLL})
        list(APPEND MISSING_DLL_PATHS MISSING_DLL_PATHS_NESTED)
        list(GET MISSING_DLL_PATHS 0 MISSING_DLL_PATH)
        if(NOT ${MISSING_DLL_PATH} STREQUAL "")
            message(${MISSING_DLL_PATH})
            add_custom_command(
                TARGET loon-cli
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${MISSING_DLL_PATH} $<TARGET_FILE_DIR:loon-cli>
                COMMAND_EXPAND_LISTS)
            break()
        endif()
    endforeach()
endforeach()
