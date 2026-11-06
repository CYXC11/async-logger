cmake_minimum_required(VERSION 3.20)

project(AsyncLogger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 1. 构建日志库
add_library(async_logger STATIC
        src/logger.cpp
)

target_include_directories(async_logger PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 2. 构建演示程序
add_executable(logger_demo
        examples/basic.cpp
)

target_link_libraries(logger_demo PRIVATE
        async_logger
)

# 3. 配置测试
include(CTest)

if(BUILD_TESTING)
    add_executable(logger_test
            tests/logger_test.cpp
    )

    target_link_libraries(logger_test PRIVATE
            async_logger
    )

    add_test(NAME logger_basic COMMAND logger_test)
endif()
