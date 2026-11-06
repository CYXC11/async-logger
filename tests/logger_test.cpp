#include "tinylog/logger.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    const std::filesystem::path path = "logger_test_output.log";

    // 删除旧测试文件，防止影响当前测试
    std::filesystem::remove(path);

    // 创建 Logger，并写入两条日志
    // 刻意多加了一层 {}，使 Logger 在读取日志文件之前析构
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

        std::cerr << "Unexpected log file contents\n";
        return 1;
        }

    in.close();
    std::filesystem::remove(path);

    std::cout << "Logger test passed\n";
    return 0;
}
