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

### Build libjpeg-turbo
```bash
git clone https://github.com/libjpeg-turbo/libjpeg-turbo.git

export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

cd libjpeg-turbo
rm -rf build-android

cmake -B build-android -S . -G"Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_ARM_MODE=arm \
  -DANDROID_PLATFORM=android-"$API" \
  -DCMAKE_ASM_FLAGS="--target=aarch64-linux-android${ANDROID_VERSION}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX/libjpeg-turbo"

cmake --build build-android -j
cmake --install build-android
```

### Build libyuv
```bash
git clone https://chromium.googlesource.com/libyuv/libyuv

export API=26
export ABI=arm64-v8a
export PREFIX=$HOME/android/third_party/$ABI

cd libyuv
rm -rf build-android

cmake -B build-android -S . -G"Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_ARM_MODE=arm \
  -DANDROID_PLATFORM=android-"$API" \
  -DCMAKE_ASM_FLAGS="--target=aarch64-linux-android${ANDROID_VERSION}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX/libyuv"

cmake --build build-android -j
cmake --install build-android
```