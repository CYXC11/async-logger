#pragma once

#include "benchmark_common.h"
#include "tinylog/logger.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <latch>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace tinylog::bench {

class LockedLogger {
public:
    explicit LockedLogger(const std::filesystem::path& path) : logger_(path) {}

    void log(Level level, std::string_view payload) {
        std::lock_guard lock(mutex_);
        logger_.log(level, payload);
    }

    void flush() {
        std::lock_guard lock(mutex_);
        logger_.flush();
    }

private:
    Logger logger_;
    std::mutex mutex_;
};

struct Timing {
    Clock::duration submit;
    Clock::duration total;
};

template <typename Logger>
Timing measure(Logger& logger, std::string_view payload, std::size_t producers,
               std::size_t count, void (Logger::*complete)(), bool timed) {
    const auto participants = static_cast<std::ptrdiff_t>(producers);
    const std::size_t logs_per_producer = count / producers;
    std::latch ready(participants);
    std::latch start_gate(1);
    std::latch logs_finished(participants);
    bool cancelled = false;
    std::vector<std::exception_ptr> errors(producers);
    std::vector<std::jthread> workers;
    workers.reserve(producers);

    try {
        for (std::size_t i = 0; i < producers; ++i) {
            workers.emplace_back([&, i] {
                ready.count_down();
                start_gate.wait();
                if (cancelled) {
                    return;
                }

                try {
                    for (std::size_t n = 0; n < logs_per_producer; ++n) {
                        logger.log(Level::Info, payload);
                    }
                } catch (...) {
                    errors[i] = std::current_exception();
                }
                logs_finished.count_down();
            });
        }
    } catch (...) {
        // Release the workers already waiting so unwinding can join them.
        cancelled = true;
        start_gate.count_down();
        throw;
    }

    const auto now = [timed] { return timed ? Clock::now() : Clock::time_point{}; };
    ready.wait();
    const auto start = now();
    start_gate.count_down();
    logs_finished.wait();
    const auto submitted = now();
    (logger.*complete)();
    const auto completed = now();

    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    return {submitted - start, completed - start};
}

template <typename Logger, void (Logger::*complete)()>
Timing run_trial(const std::filesystem::path& path, std::string_view payload,
                 std::size_t producers, std::size_t count, bool timed = true) {
    if (producers == 0 || count % producers != 0) {
        throw std::invalid_argument("Log count must divide evenly across producers");
    }

    std::filesystem::remove(path);
    Timing timing;
    {
        Logger logger(path);
        timing = measure(logger, payload, producers, count, complete, timed);
    }
    verify_line_count(path, count);
    return timing;
}

} // namespace tinylog::bench
