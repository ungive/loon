cmake_minimum_required(VERSION 3.15)
project("loon")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/cmake)

option(LOON_BUILD_EXAMPLES "Build loon examples" ON)
option(LOON_BUILD_TESTS "Build loon tests" ON)

set(SOURCES
    src/client.cpp
    src/shared_client.cpp
    src/request_handler.cpp
    src/util.cpp
    src/base64.cpp
    src/logging.cpp
    src/websocket/client.cpp
    src/websocket/backend_libdatachannel.cpp)

add_library(loon STATIC ${SOURCES})
add_library(loon::loon ALIAS loon)
target_include_directories(loon PUBLIC ./include)
target_include_directories(loon PRIVATE ./src)
set_property(TARGET loon PROPERTY CXX_STANDARD 17)
set_property(TARGET loon PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(loon_deps INTERFACE)
target_compile_definitions(loon_deps INTERFACE NOMINMAX)

# OpenSSL
find_package(OpenSSL REQUIRED)
target_link_libraries(loon_deps INTERFACE OpenSSL::SSL OpenSSL::Crypto)

# libdatachannel
block(SCOPE_FOR VARIABLES)
set(NO_WEBRTC ON CACHE BOOL "Disable WebRTC support")
set(NO_EXAMPLES ON CACHE BOOL "Disable examples")
set(NO_TESTS ON CACHE BOOL "Disable tests build")
add_subdirectory(third_party/libdatachannel EXCLUDE_FROM_ALL)
target_link_libraries(loon_deps INTERFACE LibDataChannel::LibDataChannel)
# TODO Use conan once the following PRs are merged:
# https://github.com/paullouisageneau/libdatachannel/pull/1488
# https://github.com/paullouisageneau/libdatachannel/pull/1491
# With conan "NO_WEBRTC" would have to be exposed as an option. One could most
# likely also manually set it as a CMake configure option.
endblock()

# Protobuf
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "Override option" FORCE)
find_package(Protobuf CONFIG REQUIRED)
target_include_directories(loon_deps INTERFACE ${Protobuf_INCLUDE_DIRS}
                                               ${protobuf_INCLUDE_DIRS})
target_include_directories(loon_deps INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
configure_file(../api/messages.proto
               ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto COPYONLY)
add_library(loon_proto STATIC ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto)
target_link_libraries(loon_proto protobuf::libprotobuf)
target_include_directories(loon_proto PRIVATE ${Protobuf_INCLUDE_DIRS}
                                              ${protobuf_INCLUDE_DIRS})
target_include_directories(loon_proto PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(
    LANGUAGE
    cpp
    TARGET
    loon_proto
    OUT_VAR
    PROTOLOON_SOURCES
    IMPORT_DIRS
    ${CMAKE_CURRENT_BINARY_DIR}
    OUT_VAR
    PROTOLOON_SOURCES)
set_property(SOURCE ${PROTOLOON_SOURCES} PROPERTY SKIP_AUTOGEN ON)
target_link_libraries(loon_deps INTERFACE protobuf::libprotobuf)
target_link_libraries(loon_deps INTERFACE loon_proto)

target_link_libraries(loon PRIVATE loon_deps)

if(LOON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(LOON_BUILD_TESTS)
    # For Windows: Prevent overriding the parent project's compiler/linker
    # settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    include(GoogleTest)
    enable_testing()
    set(TEST_SOURCES
        test/common/main.cpp test/client_test.cpp
        test/shared_reference_counter_test.cpp test/shared_client_test.cpp)

    # Add the sources again, so that they are recompiled with the LOON_TEST
    # flag. Setting the LOON_TEST flag for the loon library itself is not
    # desirable, as that alters the exported library. The loon library sources
    # have to be compiled with LOON_TEST defined though, otherwise there can be
    # strange alignment problems, when there is a method definition or member
    # declaration in the header, but not in the linked library!
    add_executable(loon_test ${SOURCES} ${TEST_SOURCES})
    target_link_libraries(loon_test PRIVATE loon_deps)
    target_compile_definitions(loon_test PRIVATE LOON_TEST)
    target_include_directories(loon_test PRIVATE ./include ./src ./)
    target_include_directories(loon_test PRIVATE ${Protobuf_INCLUDE_DIRS})
    target_include_directories(loon_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(loon_test PRIVATE GTest::gmock_main)
    target_include_directories(loon_test PRIVATE ${gtest_SOURCE_DIR}/include
                                                 ${gtest_SOURCE_DIR})
    find_package(CURL CONFIG REQUIRED)
    target_link_libraries(loon_test PRIVATE CURL::libcurl)
    set_property(TARGET loon_test PROPERTY CXX_STANDARD 17)
    set_property(TARGET loon_test PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

    # Ensure deterministic behaviour in tests
    target_compile_options(loon_test BEFORE PRIVATE -O0 -g)

    if(WIN32)
        add_custom_command(
            TARGET loon_test
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:loon_test> $<TARGET_FILE_DIR:loon_test>
            COMMAND_EXPAND_LISTS)

        # Copy DLLs that aren't included with TARGET_RUNTIME_DLLS for some
        # reason.
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
                file(GLOB_RECURSE MISSING_DLL_PATHS
                     ${PREFIX_PATH}/${MISSING_DLL})
                file(GLOB_RECURSE MISSING_DLL_PATHS_NESTED
                     ${PREFIX_PATH}/**/${MISSING_DLL})
                list(APPEND MISSING_DLL_PATHS MISSING_DLL_PATHS_NESTED)
                list(GET MISSING_DLL_PATHS 0 MISSING_DLL_PATH)
                if(NOT ${MISSING_DLL_PATH} STREQUAL "")
                    message(${MISSING_DLL_PATH})
                    add_custom_command(
                        TARGET loon_test
                        POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                ${MISSING_DLL_PATH} $<TARGET_FILE_DIR:loon_test>
                        COMMAND_EXPAND_LISTS)
                    break()
                endif()
            endforeach()
        endforeach()
    endif()

    add_test(loon_test loon_test)
    gtest_discover_tests(loon_test)
endif()
