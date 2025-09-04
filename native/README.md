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

### Build protobuf for host
```bash
cd grpc/third_party/protobuf
git submodule update --init --recursive

cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release -Dprotobuf_BUILD_TESTS=OFF
cmake --build build-host -j protoc

```

### Generate C++ sources from .proto
```bash
OUT_DIR=protoc
rm -rf $OUT_DIR
mkdir -p "$OUT_DIR"

PROTOC="grpc/third_party/protobuf/build-host/protoc"
PROTOC --version

PROTOC -I ../protos \
  --cpp_out="$OUT_DIR" \
  ../protos/vision.proto

PROTOC -I ../protos \
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

cmake -B build-android -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-"$API" \
  -DBUILD_SHARED_LIBS=OFF \
  -DgRPC_BUILD_TESTS=OFF -DgRPC_BUILD_CODEGEN=OFF -DgRPC_INSTALL=ON \
  -DgRPC_CARES_PROVIDER=module -DgRPC_ZLIB_PROVIDER=module \
  -DgRPC_ABSL_PROVIDER=module -DgRPC_RE2_PROVIDER=module \
  -DgRPC_PROTOBUF_PROVIDER=module \
  -DCMAKE_INSTALL_PREFIX="$PREFIX/grpc"

cmake --build build-android -j --parallel 6
cmake --install build-android
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
  -DgRPC_DIR="$PREFIX/grpc/lib/cmake/grpc"

cmake --build build-android -j
```

### Build
```bash
export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

OUTPUT="build-android"

rm -rf $OUTPUT
cmake -B $OUTPUT -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-"$API" \
  -DCMAKE_PREFIX_PATH="$PREFIX/grpc" \
  -Dabsl_DIR="$PREFIX/grpc/lib/cmake/absl" \
  -Dutf8_range_DIR="$PREFIX/grpc/lib/cmake/utf8_range" \
  -DProtobuf_DIR="$PREFIX/grpc/lib/cmake/protobuf" \
  -DgRPC_DIR="$PREFIX/grpc/lib/cmake/grpc"

cmake --build $OUTPUT -j
```

### Install into Unity project
```bash
export ABI=arm64-v8a
PLUGIN_DIR="../unity/Assets/gRPC/Plugins/Android/${ABI}"
mkdir -p $PLUGIN_DIR
cp build-android/libaiv_plugin.so $PLUGIN_DIR
```

## Test gRPC runner
### Build
```bash
export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

OUTPUT="build-android-test"

rm -rf $OUTPUT
cmake -B $OUTPUT -S . \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-"$API" \
  -DCMAKE_PREFIX_PATH="$PREFIX/grpc" \
  -Dabsl_DIR="$PREFIX/grpc/lib/cmake/absl" \
  -Dutf8_range_DIR="$PREFIX/grpc/lib/cmake/utf8_range" \
  -DProtobuf_DIR="$PREFIX/grpc/lib/cmake/protobuf" \
  -DgRPC_DIR="$PREFIX/grpc/lib/cmake/grpc"

cmake --build $OUTPUT -j
```

### Run
```powershell
BUILD_OUTPUT="build-android-test"
LOCAL_PORT=8032
REMOTE_PORT=8032

adb push "${BUILD_OUTPUT}/vision_client" /data/local/tmp/vision_client
adb push ../.local/test.png /data/local/tmp/image.png

adb shell chmod 755 /data/local/tmp/vision_client

adb reverse "tcp:${REMOTE_PORT}" "tcp:${LOCAL_PORT}"

adb shell /data/local/tmp/vision_client \
  "127.0.0.1:${REMOTE_PORT}" \
  /data/local/tmp/image.png \
  640 480 \
  --score=0.5 --deadline_ms=1000
```