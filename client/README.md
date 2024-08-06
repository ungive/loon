# Reference client implementation (C++17)

This directory contains a feature-rich reference client implementation
for use with a loon server.

## Example usage

See [`examples/client.cpp`](./examples/client.cpp)
and [`examples/CMakeLists.txt`](./examples/CMakeLists.txt)

## Dependencies

- OpenSSL (find_package)
- Protobuf (find_package)
- One websocket backend:
  - [libhv](https://github.com/ithewei/libhv) (git submodule)
  - Qt6 with Qt6::WebSockets (find_package)
- [gtest](https://github.com/google/googletest)
  (tests only, git submodule)
- curl (tests only, find_package)
- the CMake build system
- a C++17 compiler
