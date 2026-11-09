# Independent downstream consumer for the installed Aurora package.
# This is NOT registered by the main build, by CTest, or by AURORA_BUILD_VERIFY_TOOLS.
#
# How to exercise the "prebuilt library, no recompile" contract:
#   cmake --install <aurora-build-dir> --prefix <install-prefix>
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build build
#   ./build/aurora_installed_consumer
#
# The consumer only uses the umbrella header and the always-on facade. It must build and run
# when AURORA_ENABLE_PROFILING is AUTO (the macro is not exported by the installed package).

cmake_minimum_required(VERSION 3.20)
project(aurora_installed_consumer LANGUAGES CXX)

find_package(Aurora CONFIG REQUIRED)

add_executable(aurora_installed_consumer main.cpp)
target_compile_features(aurora_installed_consumer PRIVATE cxx_std_20)
target_link_libraries(aurora_installed_consumer PRIVATE Aurora::aurora)
