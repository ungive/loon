cmake_minimum_required(VERSION 3.8)
project("loon")

set(LIB_SOURCES src/client.cpp src/request_handle.cpp)

add_library(loon STATIC ${LIB_SOURCES})
target_include_directories(loon PRIVATE ./include ./src .) # TODO remove .

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
    PROTOC_OUT_DIR
    ${CMAKE_CURRENT_SOURCE_DIR} # TODO binary dir
    IMPORT_DIRS
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(loon protobuf::libprotobuf)
target_link_libraries(loon loon-proto)

set_property(TARGET loon PROPERTY CXX_STANDARD 17)
set_property(TARGET loon PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

# TODO move into separate CMakeLists.txt file:

set(EXAMPLE_CLIENT_SOURCES examples/client.cpp)

add_executable(loon-cli ${EXAMPLE_CLIENT_SOURCES})
target_link_libraries(loon-cli loon)
target_include_directories(loon-cli PRIVATE ./include .) # TODO remove .

set_property(TARGET loon-cli PROPERTY CXX_STANDARD 17)
set_property(TARGET loon-cli PROPERTY CMAKE_CXX_STANDARD_REQUIRED ON)

add_custom_command(
    TARGET loon-cli
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:loon-cli>
            $<TARGET_FILE_DIR:loon-cli>
    COMMAND_EXPAND_LISTS)
