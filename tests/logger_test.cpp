#include "tinylog/logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>

bool test_logger_created(const std::filesystem::path& path);
bool test_flush(const std::filesystem::path& path);
bool test_append_mode(const std::filesystem::path& path);
bool test_open_file_failure();

int main() {
    const std::filesystem::path path = "logger_test_output.log";

    if (!test_logger_created(path)) {
        std::cerr << "Logger create test failed\n";
        return 1;
    }

    if (!test_flush(path)) {
        std::cerr << "Logger flush test failed\n";
        return 1;
    }

    if (!test_append_mode(path)) {
        std::cerr << "Logger append mode test failed\n";
        return 1;
    }

    if (!test_open_file_failure()) {
        std::cerr << "Logger open file failure test failed\n";
        return 1;
    }

    std::cout << "Logger test passed\n";
    return 0;
}

bool test_logger_created(const std::filesystem::path& path) {
    // 删除旧测试文件，防止影响当前测试
    std::filesystem::remove(path);

    // 创建 Logger，并写入两条日志
    {
        tinylog::Logger logger(path);
        logger.log(tinylog::Level::Info, "first");
        logger.log(tinylog::Level::Error, "second");
    }

    // 重新打开文件并读取结果
    std::ifstream in(path);

    std::string first;
    std::string second;
    std::string extra;

    if (!std::getline(in, first) ||
        !std::getline(in, second) ||
        first != "[INFO] first" ||
        second != "[ERROR] second" ||
        std::getline(in, extra)) {

        std::cerr << "Unexpected log file contents when create logger\n";
        return false;
    }

    in.close();

    // 删除测试文件，防止影响后续测试
    std::filesystem::remove(path);

    return true;
}

bool test_flush(const std::filesystem::path& path) {
    // 删除旧测试文件，防止影响当前测试
    std::filesystem::remove(path);

    // 创建 Logger，并写入一条日志
    {
        tinylog::Logger logger(path);
        logger.log(tinylog::Level::Info, "flush test");
        logger.flush();

        // 在 logger 析构前，检查文件内容是否正确
        std::ifstream in(path);

        std::string flush_test;
        std::string extra;

        if (!std::getline(in, flush_test) ||
            flush_test != "[INFO] flush test" ||
            std::getline(in, extra)) {

            std::cerr << "Unexpected log file contents when flushing\n";
            return false;
            }

        in.close();
    }
    // 删除测试文件，防止影响后续测试
    std::filesystem::remove(path);

    return true;
}

bool test_append_mode(const std::filesystem::path& path) {
    // 删除旧测试文件，防止影响当前测试
    std::filesystem::remove(path);

    // 创建 Logger，并写入两条日志
    {
        tinylog::Logger logger(path);
        logger.log(tinylog::Level::Info, "first");
        logger.log(tinylog::Level::Error, "second");
    }

    // 重新打开 Logger 并写入第三条日志
    {
        tinylog::Logger logger(path);
        logger.log(tinylog::Level::Warn, "third");
    }

    // 重新打开文件并读取结果
    std::ifstream in(path);

    std::string first;
    std::string second;
    std::string third;
    std::string extra;

    if (!std::getline(in, first) ||
        !std::getline(in, second) ||
        !std::getline(in, third) ||
        first != "[INFO] first" ||
        second != "[ERROR] second" ||
        third != "[WARN] third" ||
        std::getline(in, extra)) {

        std::cerr << "Unexpected log file contents when appending\n";
        return false;
    }

    in.close();

    // 删除测试文件，防止影响后续测试
    std::filesystem::remove(path);

    return true;
}

bool test_open_file_failure()
{
    const std::filesystem::path path = "logger_nonexistent_dir/forbidden.log"; // 假设这个路径是不可写的
    if (std::filesystem::exists(path.parent_path())) {
        std::cerr << "Test path exists, please remove it to test open file failure\n";
        return false;
    }

    bool exception_caught = false;

    try {
        tinylog::Logger logger(path);
    } catch (const std::runtime_error& e) {
        std::cout << "Caught expected exception: " << e.what() << '\n';
        exception_caught = true;
    }

    return exception_caught;
}
