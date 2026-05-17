cmake_minimum_required(VERSION 3.10)
project(NebulaX VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 头文件目录
include_directories(include)

# 自动搜索 src 下所有 .cpp 源文件
file(GLOB SOURCES src/*.cpp)

# 可执行目标
add_executable(nebulaX ${SOURCES})

# 编译选项
target_compile_options(nebulaX PRIVATE
    -Wall -Wextra -Wpedantic
    -O2
    -fno-omit-frame-pointer   # perf fp unwind 需要
)
target_link_options(nebulaX PRIVATE
    -fno-omit-frame-pointer
)

# 链接线程（后续多线程用）
find_package(Threads REQUIRED)
target_link_libraries(nebulaX PRIVATE Threads::Threads)
