cmake_minimum_required(VERSION 3.21)
project(vision_client LANGUAGES CXX)

set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

find_package(utf8_range CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

add_executable(vision_client
  vision_client.cpp
  ${CMAKE_SOURCE_DIR}/protoc/vision.pb.cc
  ${CMAKE_SOURCE_DIR}/protoc/vision.grpc.pb.cc
)

target_include_directories(vision_client PRIVATE ${CMAKE_SOURCE_DIR}/protoc)

target_link_libraries(vision_client PRIVATE
  gRPC::grpc++_unsecure
  protobuf::libprotobuf
  log
)

target_compile_features(vision_client PRIVATE cxx_std_17)
