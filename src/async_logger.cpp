#include "tinylog/async_logger.h"

#ifdef TINYLOG_FLUSH_FAILURE_TEST
#include "flush_failure_test_hooks.h"
#endif

#include <iostream>
#include <stdexcept>
#include <utility>

namespace tinylog {

namespace {
std::string_view level_name(Level level) {
    switch (level) {
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }

    return "UNKNOWN";
}

struct FileFailure {
    const char* message;
};

void complete_failed_flush(std::promise<const char*>& completion) {
    try {
        throw;
    } catch (const FileFailure& failure) {
        // Give each caller its own exception and what() storage.
        completion.set_value(failure.message);
    } catch (...) {
        completion.set_exception(std::current_exception());
    }
}
} // namespace

AsyncLogger::AsyncLogger(const std::filesystem::path& path) : file_(path, std::ios::app) {

    // 检查 file_ 是否成功打开
    if (!file_) {
        throw std::runtime_error("Cannot open log file: " + path.string());
    }
    // 启动 worker_，让它执行 worker_loop()
    worker_ = std::thread(&AsyncLogger::worker_loop, this);
}

AsyncLogger::~AsyncLogger() {
    try {
        shutdown();
    } catch (...) {
        // 析构函数不向外传播异常
    }
}

void AsyncLogger::log(Level level, std::string_view message) {
    {
        // 获取 mutex_
        std::unique_lock<std::mutex> lock(mutex_);
        // 构造 LogEntry 并加入 queue_
        LogEntry logentry = {.level = level, .message = std::string(message)};

        // 等待队列有空间或者 stopping
        cv_not_full_.wait(lock, [this] { return queue_.size() < capacity_ || stopping_; });

        if (worker_failed_) {
            throw std::runtime_error("AsyncLogger worker failed");
        }

        // 检查 stopping_ 如果已经关闭，拒绝提交新日志，只有未关闭时才入队
        if (stopping_) {
            throw std::runtime_error("AsyncLogger is stopping, cannot log new messages.");
        }

        queue_.emplace(std::move(logentry));
    } // 离开作用域，释放锁

    // 通知后台线程
    cv_.notify_one();
}

void AsyncLogger::shutdown() {
    {
        // 获取互斥锁
        std::lock_guard lock(mutex_);
        // 设置 stopping_ = true
        stopping_ = true;
    }

    // 通知后台线程
    cv_.notify_one();
    cv_not_full_.notify_all();
    // 等待后台线程结束
    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::lock_guard lock(mutex_);
        if (worker_failed_) {
            throw std::runtime_error("worker have failed in worker_loop");
        }
    }
}

void AsyncLogger::flush() {
    FlushRequest request;

    // 1. 从 request.completion 获取 future
    auto fut = request.completion.get_future();
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // 2. 如果队列已满，等待空位或停止状态
        cv_not_full_.wait(lock, [this] { return queue_.size() < capacity_ || stopping_; });
        // 3. 检查 worker_failed_ 和 stopping_
        if (worker_failed_) {
            throw std::runtime_error("AsyncLogger worker failed");
        }

        if (stopping_) {
            throw std::runtime_error("cannot flush while stopping.");
        }
        // 4. 将 FlushRequest 移动入队
        queue_.emplace(std::move(request));
    }

#ifdef TINYLOG_FLUSH_FAILURE_TEST
    test::flush_enqueued();
#endif

    // 5. 通知后台消费者
    cv_.notify_one();
    // 6. 等待刷新完成
    if (const char* failure = fut.get()) {
        throw std::runtime_error(failure);
    }
}

void AsyncLogger::worker_loop() {
    try {
        while (true) {
            // 获取 mutex_
            std::unique_lock lock(mutex_);
            // 使用 cv_.wait() 等待队列非空或 stopping_
            cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            // 如果队列为空并且正在关闭 退出循环
            if (queue_.empty() && stopping_)
                break;
            // 使用结构化绑定 取出一条 LogEntry
            Task task = std::move(queue_.front());
            queue_.pop();
            // 释放 mutex_
            lock.unlock();

            // 通知一个正在等待队列空间的生产者
            cv_not_full_.notify_one();

            if (auto* entry = std::get_if<LogEntry>(&task)) {
                // 写入普通日志 将 LogEntry 写入 file_
                file_ << '[' << level_name(entry->level) << "] " << entry->message << '\n';

                // 写入失败设置错误和暂停状态
                if (!file_) {
                    throw FileFailure{"File write failed"};
                }
            } else if (auto* request = std::get_if<FlushRequest>(&task)) {
                try {
#ifdef TINYLOG_FLUSH_FAILURE_TEST
                    test::before_file_flush();
#endif
                    // 执行 file_.flush()
                    file_.flush();

                    // 检查文件流状态
                    if (!file_) {
                        throw FileFailure{"File flush failed"};
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock_error(mutex_);
                        worker_failed_ = true;
                        stopping_ = true;
                    }

                    complete_failed_flush(request->completion);
#ifdef TINYLOG_FLUSH_FAILURE_TEST
                    test::flush_exception_delivered();
#endif
                    // 重新抛出异常以便在 worker_loop 中捕获
                    throw;
                }
                // 刷新成功
                request->completion.set_value(nullptr);
            }
        }

        file_.flush();

        if (!file_) {
            throw FileFailure{"File flush failed"};
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            worker_failed_ = true;
            stopping_ = true;

            while (!queue_.empty()) {
                if (auto* request = std::get_if<FlushRequest>(&queue_.front())) {
                    complete_failed_flush(request->completion);
                }

                queue_.pop();
            }
        }

        cv_not_full_.notify_all();
    }
}

} // namespace tinylog
