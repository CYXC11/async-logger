#include "concurrent_benchmark.h"
#include "tinylog/async_logger.h"

#include <array>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace bench = tinylog::bench;

constexpr std::array<std::size_t, 4> producer_counts{1, 2, 4, 8};
constexpr std::size_t trials = 5;

void print_medians(std::string_view name, std::size_t producers,
                   const std::array<bench::Timing, trials>& timings) {
    const auto median_submit = bench::median(timings, &bench::Timing::submit);
    const auto median_total = bench::median(timings, &bench::Timing::total);

    std::cout << name << " | producers=" << producers << std::fixed << std::setprecision(3)
              << " | T_submit=" << bench::Milliseconds(median_submit).count() << " ms"
              << " | T_total=" << bench::Milliseconds(median_total).count() << " ms"
              << std::setprecision(0) << " | completed throughput="
              << bench::completed_throughput(bench::log_count, median_total) << " logs/sec"
              << " | verified runs=" << trials << '/' << trials << '\n';
}

} // namespace

int main() {
    try {
        const std::string payload(bench::payload_size, 'x');
        std::cout << "Medians of " << trials << " trials per combination, " << bench::log_count
                  << " lines verified per trial, " << payload.size() << " payload bytes\n";

        for (const auto producers : producer_counts) {
            const auto suffix = std::to_string(producers) + ".log";
            const std::filesystem::path sync_path = "tinylog_bench_multi_sync_" + suffix;
            const std::filesystem::path async_path = "tinylog_bench_multi_async_" + suffix;
            std::array<bench::Timing, trials> sync_timings;
            std::array<bench::Timing, trials> async_timings;
            for (std::size_t trial = 0; trial < trials; ++trial) {
                sync_timings[trial] = bench::run_trial<bench::LockedLogger, &bench::LockedLogger::flush>(
                    sync_path, payload, producers, bench::log_count);
                async_timings[trial] = bench::run_trial<tinylog::AsyncLogger, &tinylog::AsyncLogger::shutdown>(
                    async_path, payload, producers, bench::log_count);
            }
            print_medians("Sync (external mutex)", producers, sync_timings);
            print_medians("Async (shutdown)", producers, async_timings);
        }
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
