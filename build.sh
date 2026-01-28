
BUILD_TYPE=Debug

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

ninja -C build

