# Minimal external parent project. It pulls orderbook in with
# add_subdirectory the way a real consumer would, so a configure of this project
# proves that as a subproject orderbook defaults OB_WERROR and OB_NATIVE_ARCH OFF
# and does not impose -Werror or -march=native on the consumer's compile line.
#
# Configure it out of tree, for example:
#   cmake -S test/consumer -B out/consumer
#   cmake -S test/consumer -B out/consumer-native -DOB_NATIVE_ARCH=ON   # override wins
# then inspect out/consumer/compile_commands.json for the consumer target.
cmake_minimum_required(VERSION 3.24)
project(ob_consumer LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A real consumer only wants the library, not orderbook's tests, benchmarks,
# tools, or examples (which would also trigger the FetchContent dependencies).
set(OB_BUILD_TESTS OFF)
set(OB_BUILD_BENCH OFF)
set(OB_BUILD_TOOLS OFF)
set(OB_BUILD_EXAMPLES OFF)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/orderbook)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ob)
