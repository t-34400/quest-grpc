cmake_minimum_required(VERSION 3.21)
project(aiv_plugin LANGUAGES C CXX)

option(BUILD_ANDROID "Build for Android" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)

find_package(utf8_range CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

set(ABI $ENV{ABI})

set(LIBYUV_ROOT        "$ENV{HOME}/android/third_party/${ABI}/libyuv")
set(LIBYUV_LIB_DIR     "${LIBYUV_ROOT}/lib")
set(LIBYUV_INCLUDE_DIR "${LIBYUV_ROOT}/include")
add_library(yuv STATIC IMPORTED GLOBAL)
set_target_properties(yuv PROPERTIES
  IMPORTED_LOCATION "${LIBYUV_LIB_DIR}/libyuv.a"
  INTERFACE_INCLUDE_DIRECTORIES "${LIBYUV_INCLUDE_DIR}"
)

set(TURBOJPEG_ROOT "$ENV{HOME}/android/third_party/${ABI}/libjpeg-turbo")
set(TURBOJPEG_A    "${TURBOJPEG_ROOT}/lib/libturbojpeg.a")
set(TURBOJPEG_INC  "${TURBOJPEG_ROOT}/include")
add_library(turbojpeg_a STATIC IMPORTED GLOBAL)
set_target_properties(turbojpeg_a PROPERTIES
  IMPORTED_LOCATION "${TURBOJPEG_A}"
  INTERFACE_INCLUDE_DIRECTORIES "${TURBOJPEG_INC}"
)

set(VISION_PROTO_DIR "${CMAKE_SOURCE_DIR}/protoc" CACHE PATH
    "Directory containing vision.pb.cc and vision.grpc.pb.cc")

add_library(aiv_plugin SHARED
  aiv_plugin.cpp
  ${VISION_PROTO_DIR}/vision.pb.cc
  ${VISION_PROTO_DIR}/vision.grpc.pb.cc
)

target_include_directories(aiv_plugin PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${VISION_PROTO_DIR}
)

target_link_libraries(aiv_plugin
  PRIVATE
    gRPC::grpc++_unsecure
    protobuf::libprotobuf
    turbojpeg_a
    yuv
)

if(ANDROID)
  find_library(log-lib     log)
  find_library(camera2-lib camera2ndk)
  find_library(media-lib   mediandk)
  target_link_libraries(aiv_plugin PRIVATE
    ${log-lib}
    ${camera2-lib}
    ${media-lib}
    android
  )
endif()

set_target_properties(aiv_plugin PROPERTIES OUTPUT_NAME "aiv_plugin")
