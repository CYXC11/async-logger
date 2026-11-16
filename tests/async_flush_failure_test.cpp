#include "flush_failure_test_hooks.h"
#include "tinylog/async_logger.h"

#include <array>
#include <future>
#include <iostream>
#include <latch>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

bool test_multi_producer_backpressure(std::latch& flush_dequeued, std::latch& allow_flush);

namespace {

constexpr int flush_request_count = 3;

std::latch flushes_enqueued{flush_request_count};
std::once_flag first_flush;
std::latch first_flush_dequeued{1};
std::latch allow_file_flush{1};
std::latch allow_worker_cleanup{1};

bool flush_reports_file_failure(tinylog::AsyncLogger& logger,
                                std::string_view expected = "File flush failed") {
    try {
        logger.flush();
    } catch (const std::runtime_error& error) {
        return std::string_view(error.what()) == expected;
    } catch (...) {
        return false;
    }
    return false;
}

bool check_failed_flushes(std::span<std::future<bool>> results, std::string_view expected) {
    bool all_failed = true;
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (!results[index].get()) {
            std::cerr << "Flush waiter " << index + 1 << " did not receive \""
                      << expected << "\"\n";
            all_failed = false;
        }
    }
    return all_failed;
}

bool rejects_log_after_flush_failure() {
    tinylog::AsyncLogger logger("/dev/full");
    logger.log(tinylog::Level::Info, "Buffered before flush");
    allow_file_flush.count_down();

    const bool flush_failed = flush_reports_file_failure(logger);
    bool log_rejected = false;
    try {
        logger.log(tinylog::Level::Info, "Submitted after failed flush");
    } catch (const std::runtime_error& error) {
        log_rejected = std::string_view(error.what()) == "AsyncLogger worker failed";
    } catch (...) {
    }

    // The caller must observe the failure before the outer catch can publish it again.
    allow_worker_cleanup.count_down();

    if (!flush_failed) {
        std::cerr << "flush() did not report the background file flush failure\n";
    }
    if (!log_rejected) {
        std::cerr << "log() did not reject the submission with the worker failure\n";
    }
    return flush_failed && log_rejected;
}

bool releases_all_flush_waiters() {
    tinylog::AsyncLogger logger("/dev/full");
    logger.log(tinylog::Level::Info, "Buffered before flush");
    allow_worker_cleanup.count_down();

    const auto call_flush = [&logger] { return flush_reports_file_failure(logger); };
    std::array<std::future<bool>, flush_request_count> results;
    results.front() = std::async(std::launch::async, call_flush);
    first_flush_dequeued.wait();

    for (std::size_t index = 1; index < results.size(); ++index) {
        results[index] = std::async(std::launch::async, call_flush);
    }

    // The worker is holding the first request, so the other two must still be queued.
    flushes_enqueued.wait();
    allow_file_flush.count_down();

    return check_failed_flushes(results, "File flush failed");
}

bool releases_same_batch_waiters(std::string_view message, std::string_view expected) {
    tinylog::AsyncLogger logger("/dev/full");
    allow_worker_cleanup.count_down();

    std::future<void> warmup;
    std::array<std::future<bool>, flush_request_count - 1> results;
    try {
        warmup = std::async(std::launch::async, [&logger] { logger.flush(); });
        first_flush_dequeued.wait();

        logger.log(tinylog::Level::Info, message);
        for (auto& result : results) {
            result = std::async(std::launch::async, [&logger, expected] {
                return flush_reports_file_failure(logger, expected);
            });
        }

        // The empty flush pins the worker, so the log and both waiters enter the next batch together.
        flushes_enqueued.wait();
    } catch (...) {
        allow_file_flush.count_down();
        throw;
    }
    allow_file_flush.count_down();

    try {
        warmup.get();
    } catch (const std::exception& error) {
        std::cerr << "Empty warmup flush failed: " << error.what() << '\n';
        return false;
    }
    return check_failed_flushes(results, expected);
}

} // namespace

namespace tinylog::test {

void flush_enqueued() {
    flushes_enqueued.count_down();
}

void before_file_flush() {
    std::call_once(first_flush, [] {
        first_flush_dequeued.count_down();
        allow_file_flush.wait();
    });
}

void flush_exception_delivered() {
    allow_worker_cleanup.wait();
}

} // namespace tinylog::test

int main(int argc, char* argv[]) {
    if (argc == 2) {
        const std::string_view test_case = argv[1];
        if (test_case == "rejects_log") {
            return rejects_log_after_flush_failure() ? 0 : 1;
        }
        if (test_case == "releases_waiters") {
            return releases_all_flush_waiters() ? 0 : 1;
        }
        if (test_case == "releases_same_batch_flush_failure") {
            return releases_same_batch_waiters("Buffered before flush", "File flush failed") ? 0 : 1;
        }
        if (test_case == "releases_same_batch_write_failure") {
            return releases_same_batch_waiters(std::string(64 * 1024, 'x'), "File write failed") ? 0 : 1;
        }
        if (test_case == "backpressure") {
            allow_worker_cleanup.count_down();
            return test_multi_producer_backpressure(first_flush_dequeued, allow_file_flush) ? 0 : 1;
        }
    }

    std::cerr << "Usage: async_flush_failure_test "
                 "{rejects_log|releases_waiters|releases_same_batch_flush_failure|"
                 "releases_same_batch_write_failure|backpressure}\n";
    return 1;
}
