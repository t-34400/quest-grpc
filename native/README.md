## Setup

### Generate C++ sources from .proto
```bash
OUT_DIR=protoc
rm -rf $OUT_DIR
mkdir -p "$OUT_DIR"

PROTOC="third_party/grpc/third_party/protobuf/build-host/protoc"
$PROTOC --version

$PROTOC -I ../protos \
  --cpp_out="$OUT_DIR" \
  ../protos/vision.proto

$PROTOC -I ../protos \
  --grpc_out="$OUT_DIR" \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
  ../protos/vision.proto
```

### Build native plugins
```bash
export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

rm -rf build-android
cmake -B build-android -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-"$API" \
  -DCMAKE_PREFIX_PATH="$PREFIX/grpc" \
  -Dabsl_DIR="$PREFIX/grpc/lib/cmake/absl" \
  -Dutf8_range_DIR="$PREFIX/grpc/lib/cmake/utf8_range" \
  -DProtobuf_DIR="$PREFIX/grpc/lib/cmake/protobuf" \
  -DgRPC_DIR="$PREFIX/grpc/lib/cmake/grpc" \
  -Dlibjpeg-turbo_DIR="$PREFIX/libjpeg-turbo/lib/cmake/libjpeg-turbo"

cmake --build build-android -j
```

### Install into Unity project
```bash
export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

PLUGIN_DIR="../unity/Assets/gRPC/Plugins/Android/${ABI}"

mkdir -p $PLUGIN_DIR
cp build-android/libaiv_plugin.so $PLUGIN_DIR
```