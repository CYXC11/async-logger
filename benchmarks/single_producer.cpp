#include "benchmark_common.h"
#include "tinylog/async_logger.h"
#include "tinylog/logger.h"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace bench = tinylog::bench;

template <typename Logger>
void benchmark(std::string_view name, const std::filesystem::path& path,
               std::size_t count, void (Logger::*complete)()) {
    using bench::Clock;

    std::filesystem::remove(path);
    const std::string payload(bench::payload_size, 'x');
    Clock::time_point start;
    Clock::time_point submitted;
    Clock::time_point completed;
    {
        Logger logger(path);
        start = Clock::now();
        for (std::size_t i = 0; i < count; ++i) {
            logger.log(tinylog::Level::Info, payload);
        }
        submitted = Clock::now();
        (logger.*complete)();
        completed = Clock::now();
    }

    bench::verify_line_count(path, count);

    const auto submit_time = submitted - start;
    const auto total_time = completed - start;
    const double logs_per_second = bench::completed_throughput(count, total_time);

    std::cout << name << " (" << count << '/' << count << " lines verified)\n"
              << std::fixed << std::setprecision(3)
              << "  T_submit (log calls): " << bench::Milliseconds(submit_time).count() << " ms\n"
              << "  T_total (through completion): " << bench::Milliseconds(total_time).count() << " ms\n"
              << std::setprecision(0)
              << "  Completed throughput (count / T_total): " << logs_per_second << " logs/sec\n";
}

void benchmark_sync(std::size_t count) {
    benchmark("Sync (flush)", "tinylog_bench_sync.log", count, &tinylog::Logger::flush);
}

void benchmark_async(std::size_t count) {
    benchmark("Async (shutdown)", "tinylog_bench_async.log", count, &tinylog::AsyncLogger::shutdown);
}

} // namespace

int main() {
    constexpr std::size_t count = bench::log_count;

    try {
        std::cout << "Single producer, " << count << " logs per logger, " << bench::payload_size
                  << " payload bytes\n";
        benchmark_sync(count);
        benchmark_async(count);
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
