#include "tinylog/async_logger.h"

#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <filesystem>
#include <thread>
#include <vector>
#include <stdexcept>

bool test_async_logging();
bool test_multi_producer();
bool test_shutdown();
bool test_worker_failure();
bool test_flush();

int main() {
    if (!test_async_logging()) {
        std::cerr << "Async Logging test failed\n";
        return 1;
    }
    std::cout << "Async Logging test passed\n";

    if (!test_multi_producer()) {
        std::cerr << "Multi Producer test failed\n";
        return 1;
    }
    std::cout << "Multi Producer test passed\n";

    if (!test_shutdown()) {
        std::cerr << "Shutdown test failed\n";
        return 1;
    }
    std::cout << "Shutdown test passed\n";

    if (!test_worker_failure()) {
        std::cerr << "Worker Failure test failed\n";
        return 1;
    }
    std::cout << "Worker Failure test passed\n";

    if (!test_flush()) {
        std::cerr << "Flush test failed\n";
        return 1;
    }
    std::cout << "Flush test passed\n";


    return 0;
}


bool test_async_logging() {
    const std::filesystem::path path = "async_test.log";

    // 清理测试文件
    std::filesystem::remove(path);

    {
        tinylog::AsyncLogger logger(path);

        // 提交 1000 条带编号的消息
        for (int i = 0; i < 1000; ++i) {
            logger.log(
                tinylog::Level::Info,
                "Message " + std::to_string(i)
                );
        }
    }

    // Logger 已经析构 重新打开文件
    std::ifstream in(path);

    // 逐行读取，检查：
    // - 必须恰好有 1000 条
    // - 内容必须正确
    // - 顺序必须是 0 到 999
    for (int i = 0; i < 1000; ++i) {
        std::string line;

        if (!getline(in, line) || line != "[INFO] Message " + std::to_string(i)) {
            std::cerr << "Unexpected log file contents when create 1000 lines async logger\n";
            return false;
        }
    }

    // 检查是否存在多余日志
    std::string extra;

    if (std::getline(in, extra)) {
        std::cerr << "Unexpected extra log entry\n";
        return false;
    }

    in.close();
    // 清理测试文件
    std::filesystem::remove(path);
    return true;
}

bool test_multi_producer() {
    constexpr int thread_count = 4;
    constexpr int messages_per_thread = 1000;
    const std::filesystem::path path = "async_test.log";

    // 清理测试文件
    std::filesystem::remove(path);

    {
        tinylog::AsyncLogger logger(path);
        std::vector<std::thread> threads;

        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([i, &logger] {
                // 提交 1000 条带编号的日志
                for (int j = 0; j < messages_per_thread; ++j) {
                    logger.log(
                        tinylog::Level::Info,
                        "Thread " + std::to_string(i) + " Message " + std::to_string(j)
                        );
                }
            });
        }

        // join 所有生产者线程
        for (std::thread& thread : threads) {
            thread.join();
        }
    }

    // Logger 已经析构 读取文件并验证 4000 条日志

    // 每个线程下一条应该出现的消息编号
    std::array<int, thread_count> next_expected{};

    std::ifstream in(path);
    if (!in) {
        return false;
    }

    std::string line;
    int total_lines = 0;

    while (std::getline(in, line)) {
        std::istringstream iss(line);

        std::string level;
        std::string thread_word;
        std::string message_word;
        std::string extra;

        int thread_id;
        int message_id;

        // 解析并验证日志格式
        if (!(iss >> level >> thread_word >> thread_id
              >> message_word >> message_id) ||
            level != "[INFO]" ||
            thread_word != "Thread" ||
            message_word != "Message" ||
            (iss >> extra)) {

            std::cerr << "Invalid log format: " << line << '\n';
            return false;
        }

        // 检查线程编号是否合法
        if (thread_id < 0 || thread_id >= thread_count) {
            std::cerr << "Invalid thread ID: " << thread_id << '\n';
            return false;
        }

        // 检查消息编号和顺序
        if (message_id != next_expected[thread_id]) {
            std::cerr << "Unexpected message: " << line << '\n';
            return false;
        }

        ++next_expected[thread_id];
        ++total_lines;
    }

    // 检查读取过程有没有发生 I/O 错误
    if (in.bad()) {
        std::cerr << "I/O error while reading log file\n";
        return false;
    }

    // 检查每个线程是否都提交了完整的 1000 条消息
    for (int i = 0; i < thread_count; ++i) {
        if (next_expected[i] != messages_per_thread) {
            std::cerr << "Thread " << i
                << ": expected " << messages_per_thread
                << " messages, got " << next_expected[i]
                << '\n';
            return false;
        }
    }

    // 检查总行数
    if (total_lines != thread_count * messages_per_thread) {
        std::cerr << "Total lines mismatch: expected "
            << (thread_count * messages_per_thread)
            << ", got " << total_lines
            << '\n';
        return false;
    }

    in.close();
    std::filesystem::remove(path);

    return true;
}

bool test_shutdown() {
    const std::filesystem::path path = "async_test.log";
    bool expected_exception_caught = false;

    // 清理测试文件
    std::filesystem::remove(path);

    {
        tinylog::AsyncLogger logger(path);

        // 提交一些日志
        for (int i = 0; i < 10; ++i) {
            logger.log(tinylog::Level::Info, "Message " + std::to_string(i));
        }

        // 调用 shutdown
        logger.shutdown();

        // Logger 已经析构，检查日志文件内容
        std::ifstream in(path);
        if (!in) {
            std::cerr << "Failed to open log file after shutdown test\n";
            return false;
        }

        for (int i = 0; i < 10; ++i) {
            std::string line;

            if (!std::getline(in, line) || line != "[INFO] Message " + std::to_string(i)) {
                return false;
            }
        }

        std::string extra;
        if (std::getline(in, extra)) {
            return false;
        }

        in.close();

        // 测试重复关闭不会抛出异常
        try {
            logger.shutdown();
        } catch (...) {
            std::cerr << "Repeated shutdown should not throw an exception\n";
            return false;
        }

        // 尝试在 shutdown 后提交日志，应该抛出异常
        try {
            logger.log(tinylog::Level::Info, "This should fail");
            std::cerr << "Expected exception after shutdown, but log() succeeded\n";
            return false;
        } catch (const std::runtime_error& e) {
            // 预期的异常
            std::cout << "Caught expected exception after shutdown: " << e.what() << '\n';
            expected_exception_caught = true;
        } catch (...) {
            std::cerr << "Caught unexpected exception type after shutdown\n";
            return false;
        }
    }

    std::filesystem::remove(path);

    return expected_exception_caught;
}

bool test_worker_failure() {
    tinylog::AsyncLogger logger("/dev/full");

    logger.log(
        tinylog::Level::Info,
        "This write should fail"
    );

    bool caught = false;
    bool caught_again = false;

    try {
        logger.shutdown();
    } catch (const std::runtime_error& e) {
        std::cout << "Expected: " << e.what() << '\n';
        caught = true;
    }

    try {
        logger.shutdown();
    } catch (const std::runtime_error& e) {
        std::cout << "Expected: " << e.what() << '\n';
        caught_again = true;
    }

    return caught && caught_again;
}

bool test_flush() {
    const std::filesystem::path path = "async_test.log";

    std::filesystem::remove(path);

    tinylog::AsyncLogger logger(path);

    logger.flush(); // 队列最初为空，不应该卡住

    logger.log(tinylog::Level::Info, "Before flush");

    logger.flush();

    // 注意：此时 logger 仍然存活
    // 立即打开文件，检查是否已经能读取到 "Before flush"
    std::ifstream in(path);
    std::string line;
    if (!getline(in, line) || line != "[INFO] Before flush") {
        std::cerr << "Expected line \"[INFO] Before flush\" not found in log file\n";
        return false;
    }

    std::string extra;
    if (getline(in, extra)) {
        std::cerr << "Unexpected extra log entry after first flush\n";
        return false;
    }

    in.close();

    logger.log(tinylog::Level::Info, "After flush");

    logger.flush();
    std::ifstream in_after(path);
    std::string line1;
    std::string line2;
    if (!getline(in_after, line1) || line1 != "[INFO] Before flush") {
        std::cerr << "Expected line \"[INFO] Before flush\" not found in log file\n";
        return false;
    }
    if (!getline(in_after, line2) || line2 != "[INFO] After flush") {
        std::cerr << "Expected line \"[INFO] After flush\" not found in log file\n";
        return false;
    }

    std::string extra_after;
    if (getline(in_after, extra_after)) {
        std::cerr << "Unexpected extra log entry after second flush\n";
        return false;
    }
    in_after.close();

    return true;
}