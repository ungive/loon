cmake_minimum_required(VERSION 3.8)
project("loon")

option(LOON_BUILD_EXAMPLES "Build loon examples" ON)
option(LOON_BUILD_TESTS "Build loon tests" ON)

set(LIB_SOURCES src/client.cpp src/request_handle.cpp src/impl.cpp)

add_library(loon STATIC ${LIB_SOURCES})
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
add_library(loon-proto STATIC ${CMAKE_CURRENT_BINARY_DIR}/loon/messages.proto)
target_include_directories(loon-proto PRIVATE ${Protobuf_INCLUDE_DIRS})
target_include_directories(loon-proto PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
protobuf_generate(
    LANGUAGE
    cpp
    TARGET
    loon-proto
    OUT_VAR
    PROTOLOON_SOURCES
    IMPORT_DIRS
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(loon protobuf::libprotobuf)
target_link_libraries(loon loon-proto)

set_property(TARGET loon PROPERTY CXX_STANDARD 17)
set_property(TARGET loon PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

if(LOON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(LOON_BUILD_TESTS)
    # TODO
endif()
