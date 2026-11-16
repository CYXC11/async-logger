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

    // 预分配队列容量，避免频繁的内存分配
    queue_.reserve(capacity_);

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
    // 锁外构造 LogEntry
    LogEntry logentry = {.level = level, .message = std::string(message)};
    // 获取 mutex_
    std::unique_lock<std::mutex> lock(mutex_);
    // 等待队列有空间或者 stopping
    cv_not_full_.wait(lock, [this] { return queue_.size() < capacity_ || stopping_; });

    if (worker_failed_) {
        throw std::runtime_error("AsyncLogger worker failed");
    }

    // 检查 stopping_ 如果已经关闭，拒绝提交新日志，只有未关闭时才入队
    if (stopping_) {
        throw std::runtime_error("AsyncLogger is stopping, cannot log new messages.");
    }

    // 仅当队列为空，设置 should_notify 为 true
    const bool should_notify = queue_.empty();
    queue_.emplace_back(std::move(logentry));

    // 显式解锁，避免在通知时持有锁
    lock.unlock();

    // 当队列为空时，才通知后台线程
    if (should_notify) {
        cv_.notify_one();
    }
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
        queue_.emplace_back(std::move(request));
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
    std::vector<Task> batch;
    batch.reserve(capacity_);
    std::size_t next_index = 0;

    try {
        while (true) {
            // 获取 mutex_
            std::unique_lock lock(mutex_);
            // 使用 cv_.wait() 等待队列非空或 stopping_
            cv_.wait(lock, [this] { return !queue_.empty() || stopping_; });
            // 如果队列为空并且正在关闭 退出循环
            if (queue_.empty() && stopping_)
                break;

            const bool was_full = (queue_.size() >= capacity_);
            // 从队列中批量取出 LogEntry，并将其移动到 batch 中
            std::swap(batch, queue_);
            // 释放 mutex_
            lock.unlock();

            // 通知所有正在等待队列空间的生产者
            if (was_full) {
                cv_not_full_.notify_all();
            }

            while (next_index < batch.size()) {
                // 使用后自增，获取当前任务并递增 next_index
                auto& task = batch[next_index++];
                if (const auto* entry = std::get_if<LogEntry>(&task)) {
                    // 写入日志
                    // 写入普通日志 将 LogEntry 写入 file_
                    file_ << '[' << level_name(entry->level) << "] " << entry->message << '\n';

                    // 写入失败设置错误和暂停状态
                    if (!file_) {
                        throw FileFailure{"File write failed"};
                    }
                } else if (auto* request = std::get_if<FlushRequest>(&task)) {
                    // 刷新请求
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

            // 当前 batch处理完后，重置 next_index 并清空 batch
            next_index = 0;
            batch.clear();
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

            // 处理当前 batch 中未完成的 FlushRequest
            for (; next_index < batch.size(); ++next_index) {
                if (auto* request = std::get_if<FlushRequest>(&batch[next_index])) {
                    complete_failed_flush(request->completion);
                }
            }

            batch.clear();

            // 处理队列中未完成的 FlushRequest
            for (auto& task : queue_) {
                if (auto* request = std::get_if<FlushRequest>(&task)) {
                    complete_failed_flush(request->completion);
                }
            }

            queue_.clear();
        }

        cv_not_full_.notify_all();
    }
}

} // namespace tinylog