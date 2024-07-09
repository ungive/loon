cmake_minimum_required(VERSION 3.8)
project("loon")

option(LOON_BUILD_EXAMPLES "Build loon examples" ON)
option(LOON_BUILD_TESTS "Build loon tests" ON)

set(LIB_SOURCES src/client.cpp src/request_handler.cpp src/util.cpp)

add_library(loon STATIC ${LIB_SOURCES})
add_library(loon::loon ALIAS loon)
target_compile_definitions(loon PRIVATE NOMINMAX)
target_include_directories(loon PRIVATE ./include ./src)

# openssl
find_package(OpenSSL REQUIRED)
target_link_libraries(loon OpenSSL::SSL OpenSSL::Crypto)

# libhv
find_package(libhv CONFIG REQUIRED)
target_link_libraries(loon hv)

# protobuf + protocol buffers
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "Override option" FORCE)
find_package(Protobuf CONFIG REQUIRED)
target_include_directories(loon PRIVATE ${Protobuf_INCLUDE_DIRS})
target_include_directories(loon PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
configure_file(../../api/messages.proto
               ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto COPYONLY)
add_library(loon_proto STATIC ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto)
target_include_directories(loon_proto PRIVATE ${Protobuf_INCLUDE_DIRS})
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
target_link_libraries(loon protobuf::libprotobuf)
target_link_libraries(loon loon_proto)

set_property(TARGET loon PROPERTY CXX_STANDARD 17)
set_property(TARGET loon PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(LOON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(LOON_BUILD_TESTS)
    # For Windows:
    # Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    include(GoogleTest)
    enable_testing()
    set(TEST_SOURCES test/client_test.cpp)

    add_executable(loon_test ${TEST_SOURCES})
    target_link_libraries(loon_test loon)
    target_include_directories(loon_test PRIVATE ./include ./src)
    target_include_directories(loon_test PRIVATE ${Protobuf_INCLUDE_DIRS})
    target_include_directories(loon_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(loon_test protobuf::libprotobuf)
    target_link_libraries(loon_test loon_proto)
    target_link_libraries(loon_test GTest::gmock_main)
    target_include_directories(loon_test PRIVATE ${gtest_SOURCE_DIR}/include
                                                 ${gtest_SOURCE_DIR})
    find_package(CURL CONFIG REQUIRED)
    target_link_libraries(loon_test CURL::libcurl)
    set_property(TARGET loon_test PROPERTY CXX_STANDARD 17)
    set_property(TARGET loon_test PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

    add_custom_command(
        TARGET loon_test
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory_if_different
            $<TARGET_RUNTIME_DLL_DIRS:loon_test> $<TARGET_FILE_DIR:loon_test>
        COMMAND_EXPAND_LISTS)

    add_test(loon_test loon_test)
    gtest_discover_tests(loon_test)
endif()
