cmake_minimum_required(VERSION 3.8)
project("loon")

option(LOON_BUILD_EXAMPLES "Build loon examples" ON)
option(LOON_BUILD_TESTS "Build loon tests" ON)
set(LOON_WEBSOCKET_BACKEND "LIBHV" CACHE STRING "Websocket backend")
set_property(CACHE LOON_WEBSOCKET_BACKEND PROPERTY STRINGS "LIBHV" "QT")

set(LIB_SOURCES src/client.cpp src/request_handler.cpp src/util.cpp
                src/logging.cpp src/websocket/client.cpp)
if(LOON_WEBSOCKET_BACKEND STREQUAL "LIBHV")
    message("Using libhv as websocket backend")
    list(APPEND LIB_SOURCES src/websocket/backend_libhv.cpp)
    set(USE_LIBHV true)
elseif(LOON_WEBSOCKET_BACKEND STREQUAL "QT")
    message("Using QT as websocket backend")
    set(CMAKE_AUTOMOC ON)
    list(APPEND LIB_SOURCES src/websocket/backend_qt.cpp)
    list(APPEND LIB_SOURCES src/websocket/backend_qt.h)
    set(USE_QT true)
else()
    message(FATAL_ERROR "Wrong value of the option 'LOON_WEBSOCKET_BACKEND'")
endif()

add_library(loon STATIC ${LIB_SOURCES})
add_library(loon::loon ALIAS loon)
target_compile_definitions(loon PRIVATE NOMINMAX)
target_include_directories(loon PUBLIC ./include)
target_include_directories(loon PRIVATE ./src)

# openssl
find_package(OpenSSL REQUIRED)
target_link_libraries(loon OpenSSL::SSL OpenSSL::Crypto)

# libhv
if(DEFINED USE_LIBHV)
    include(FetchContent)
    set(WITH_OPENSSL ON CACHE INTERNAL "use openssl")
    FetchContent_Declare(libhv SOURCE_DIR
                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libhv)
    FetchContent_MakeAvailable(libhv)
    target_link_libraries(loon hv_static)
    target_compile_definitions(loon PRIVATE USE_LIBHV)
endif()

# qt websockets
if(DEFINED USE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)
    target_link_libraries(loon Qt6::Core Qt6::Network Qt6::WebSockets)
    target_compile_definitions(loon PRIVATE USE_QT)
endif()

# protobuf + protocol buffers
set(Protobuf_USE_STATIC_LIBS ON CACHE BOOL "Override option" FORCE)
find_package(Protobuf CONFIG REQUIRED)
target_include_directories(loon PRIVATE ${Protobuf_INCLUDE_DIRS} ${protobuf_INCLUDE_DIRS})
target_include_directories(loon PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
configure_file(../api/messages.proto
               ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto COPYONLY)
add_library(loon_proto STATIC ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto)
target_include_directories(loon_proto PRIVATE ${Protobuf_INCLUDE_DIRS} ${protobuf_INCLUDE_DIRS})
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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Compile test methods and functions with debug builds.
    # That way they are not greyed out during development.
    target_compile_definitions(loon PRIVATE LOON_TEST)
endif()

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
    set(TEST_SOURCES test/main.cpp test/client_test.cpp)

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
    target_compile_definitions(loon_test PRIVATE LOON_TEST)

    # Required because QWebSockets needs a QApplication instance.
    if(DEFINED USE_QT)
        find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)
        target_link_libraries(loon_test Qt6::Core Qt6::Network Qt6::WebSockets)
        target_compile_definitions(loon_test PRIVATE USE_QT)

        add_custom_command(
            TARGET loon_test
            POST_BUILD
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:loon_test> $<TARGET_FILE_DIR:loon_test>
            COMMAND_EXPAND_LISTS)
    endif()

    add_custom_command(
        TARGET loon_test
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:loon_test> $<TARGET_FILE_DIR:loon_test>
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
                    TARGET loon_test
                    POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            ${MISSING_DLL_PATH} $<TARGET_FILE_DIR:loon_test>
                    COMMAND_EXPAND_LISTS)
                break()
            endif()
        endforeach()
    endforeach()

    add_test(loon_test loon_test)
    gtest_discover_tests(loon_test)
endif()
