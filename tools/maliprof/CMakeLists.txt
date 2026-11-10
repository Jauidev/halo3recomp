# Mali GPU counter sampler for adb shell. Build: bash tools/maliprof/build.sh
cmake_minimum_required(VERSION 3.13.5)
project(maliprof CXX)
set(HWCPIPE_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/hwcpipe hwcpipe)
add_executable(maliprof maliprof.cpp)
target_link_libraries(maliprof hwcpipe device)
