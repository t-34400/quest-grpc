## Setup

### Install gRPC plugin
```bash
git clone -b v1.74.1 https://github.com/grpc/grpc
cd grpc
git submodule update --init

mkdir -p cmake/build
cd cmake/build

cmake -DgRPC_BUILD_TESTS=OFF ../..
cmake --build . --target grpc_cpp_plugin -j

mkdir -p ~/.local/bin
cp grpc_cpp_plugin ~/.local/bin/

which grpc_cpp_plugin
```

### Generate C++ sources from .proto
```bash
OUT_DIR=protoc
mkdir -p "$OUT_DIR"

protoc -I ../protos \
  --cpp_out="$OUT_DIR" \
  ../protos/vision.proto

protoc -I ../protos \
  --grpc_out="$OUT_DIR" \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  ../protos/vision.proto
```

### Build gRPC
```bash
export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

cd grpc
rm -rf build-android

cmake_compat -B build-android -S . \
  -DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-"$API" \
  -DBUILD_SHARED_LIBS=OFF \
  -DgRPC_BUILD_TESTS=OFF -DgRPC_BUILD_CODEGEN=OFF -DgRPC_INSTALL=ON \
  -DgRPC_CARES_PROVIDER=module -DgRPC_ZLIB_PROVIDER=module \
  -DgRPC_ABSL_PROVIDER=module -DgRPC_RE2_PROVIDER=module \
  -DgRPC_PROTOBUF_PROVIDER=module \
  -DCMAKE_INSTALL_PREFIX="$PREFIX/grpc"

cmake_compat --build build-android -j
cmake_compat --install build-android
```