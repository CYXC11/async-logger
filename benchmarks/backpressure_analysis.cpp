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

constexpr std::array<std::size_t, 2> producer_counts{1, 8};
constexpr std::array<std::size_t, 5> log_counts{512, 1024, 2048, 8192, bench::log_count};
constexpr std::size_t trials = 9;
using Samples = std::array<bench::Timing, trials>;

struct Mode {
    std::string_view label;
    std::string_view file_key;
    bench::Timing (*run)(const std::filesystem::path&, std::string_view, std::size_t, std::size_t, bool);
};

constexpr std::array modes{
    Mode{"Sync (external mutex, flush)", "sync_flush",
         bench::run_trial<bench::LockedLogger, &bench::LockedLogger::flush>},
    Mode{"Async (shutdown)", "async_shutdown",
         bench::run_trial<tinylog::AsyncLogger, &tinylog::AsyncLogger::shutdown>},
    Mode{"Async (flush)", "async_flush",
         bench::run_trial<tinylog::AsyncLogger, &tinylog::AsyncLogger::flush>},
};

void print_header(std::size_t producers) {
    std::cout << "\nProducers = " << producers << '\n'
              << std::right << std::setw(8) << "Logs" << "  "
              << std::left << std::setw(30) << "Logger" << std::right
              << std::setw(14) << "T_submit (ms)" << std::setw(14) << "T_total (ms)"
              << std::setw(14) << "T_tail (ms)" << std::setw(11) << "Submit %"
              << std::setw(13) << "Logs/sec" << '\n';
}

void print_medians(std::size_t count, std::string_view label, const Samples& samples) {
    const auto submit = bench::median(samples, &bench::Timing::submit);
    const auto total = bench::median(samples, &bench::Timing::total);
    const auto tail = bench::median(samples, [](const bench::Timing& timing) {
        return timing.total - timing.submit;
    });
    const auto share = bench::median(samples, [](const bench::Timing& timing) {
        return 100.0 * bench::Milliseconds(timing.submit).count() / bench::Milliseconds(timing.total).count();
    });

    std::cout << std::right << std::setw(8) << count << "  "
              << std::left << std::setw(30) << label << std::right
              << std::fixed << std::setprecision(3)
              << std::setw(14) << bench::Milliseconds(submit).count()
              << std::setw(14) << bench::Milliseconds(total).count()
              << std::setw(14) << bench::Milliseconds(tail).count()
              << std::setprecision(2) << std::setw(11) << share
              << std::setprecision(0) << std::setw(13) << bench::completed_throughput(count, total) << '\n';
}

void run_workload(std::size_t producers, std::size_t count, std::string_view payload) {
    const auto suffix = "_p" + std::to_string(producers) + "_n" + std::to_string(count) + ".log";
    std::array<std::filesystem::path, modes.size()> paths;
    std::array<Samples, modes.size()> samples;

    for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        paths[mode] = "tinylog_bench_backpressure_" + std::string(modes[mode].file_key) + suffix;
        modes[mode].run(paths[mode], payload, producers, count, false);
    }

    for (std::size_t trial = 0; trial < trials; ++trial) {
        // Rotate the order so each mode runs equally often in each position.
        for (std::size_t offset = 0; offset < modes.size(); ++offset) {
            const auto mode = (trial + offset) % modes.size();
            samples[mode][trial] = modes[mode].run(paths[mode], payload, producers, count, true);
        }
    }

    for (std::size_t mode = 0; mode < modes.size(); ++mode) {
        print_medians(count, modes[mode].label, samples[mode]);
    }
}

} // namespace

int main() {
    try {
        const std::string payload(bench::payload_size, 'x');
        std::cout << "1 untimed warmup and " << trials << " measured trials per combination. "
                  << payload.size() << "-byte payload. Every trial verifies its line count.\n"
                  << "Per-column medians with tail and submit share computed for each trial.\n";

        for (const auto producers : producer_counts) {
            print_header(producers);
            for (const auto count : log_counts) {
                run_workload(producers, count, payload);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
