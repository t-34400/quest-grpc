cmake_minimum_required(VERSION 3.21)
project(aiv_plugin LANGUAGES C CXX)

option(BUILD_ANDROID "Build for Android" ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(utf8_range CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(VISION_PROTO_DIR "${CMAKE_SOURCE_DIR}/protoc" CACHE PATH "Directory containing vision.pb.cc and vision.grpc.pb.cc")

add_library(aiv_plugin SHARED
  aiv_plugin.cpp
  ${VISION_PROTO_DIR}/vision.pb.cc
  ${VISION_PROTO_DIR}/vision.grpc.pb.cc
)

target_include_directories(aiv_plugin PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${VISION_PROTO_DIR}
)

target_link_libraries(aiv_plugin PRIVATE
  gRPC::grpc++_unsecure
  protobuf::libprotobuf
)

if(ANDROID)
  find_library(log-lib log)
  find_library(camera2-lib camera2ndk)
  find_library(media-lib mediandk)
  target_link_libraries(aiv_plugin PRIVATE
    ${log-lib}
    ${camera2-lib}
    ${media-lib}
    android
  )
endif()

set_target_properties(aiv_plugin PROPERTIES OUTPUT_NAME "aiv_plugin")
