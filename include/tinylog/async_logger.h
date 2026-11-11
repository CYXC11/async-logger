#pragma once

#include "tinylog/logger.h"

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <future>

namespace tinylog {

class AsyncLogger {
public:
    explicit AsyncLogger(const std::filesystem::path& path);
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void log(Level level, std::string_view message);
    void shutdown();
    void flush();

private:
    struct LogEntry {
        Level level;
        std::string message;
    };

    struct FlushRequest {
        std::promise<const char*> completion;
    };

    using Task = std::variant<FlushRequest, LogEntry>;

    void worker_loop();

    std::ofstream file_;
    std::queue<Task> queue_;

    std::mutex mutex_;
    std::condition_variable cv_;
    static constexpr std::size_t capacity_ = 1024;
    std::condition_variable cv_not_full_;

    bool worker_failed_ = false;
    bool stopping_ = false;

    std::thread worker_;
};

} // namespace tinylog
