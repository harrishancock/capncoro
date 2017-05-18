# Toy project to investigate N4663 Coroutines TS integration with libkj-async.
#
# To build, put capnp.exe on the PATH and run:
#
#   cd capncoro
#   cmake -H. -Bbuild -G "Visual Studio 15 2017"
#   cmake --build build --config debug
#
# You should find coroutine-test.exe under build/Debug.

project("Cap'n Coroutine" CXX)
cmake_minimum_required(VERSION 3.1)

find_package(CapnProto CONFIG REQUIRED)

add_executable(coroutine-test coroutine-test.c++)
target_link_libraries(coroutine-test
  PRIVATE
    CapnProto::kj-http
    CapnProto::kj-test
)
target_compile_features(coroutine-test PRIVATE cxx_generic_lambdas)  # Use C++14
if(MSVC)
  target_compile_options(coroutine-test PRIVATE /await)
else()
  target_compile_options(coroutine-test PRIVATE -fcoroutines-ts)
endif()

enable_testing()
add_test(NAME coroutine-test-run COMMAND coroutine-test)
