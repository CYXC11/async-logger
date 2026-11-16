#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ratio>
#include <stdexcept>
#include <string>

namespace tinylog::bench {

inline constexpr std::size_t log_count = 100'000;
inline constexpr std::size_t payload_size = 64;

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

inline double completed_throughput(std::size_t count, Clock::duration total_time) {
    return static_cast<double>(count) / std::chrono::duration<double>(total_time).count();
}

template <typename Sample, std::size_t size, typename Projection>
auto median(std::array<Sample, size> samples, Projection projection) {
    static_assert(size % 2 == 1);
    std::ranges::sort(samples, {}, projection);
    return std::invoke(projection, samples[size / 2]);
}

inline void verify_line_count(const std::filesystem::path& path, std::size_t expected) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open benchmark log: " + path.string());
    }

    std::size_t lines = 0;
    std::string line;
    while (std::getline(file, line)) {
        ++lines;
    }
    if (file.bad() || !file.eof()) {
        throw std::runtime_error("Cannot read benchmark log: " + path.string());
    }
    if (lines != expected) {
        throw std::runtime_error(path.string() + " has " + std::to_string(lines) +
                                 " lines, expected " + std::to_string(expected));
    }
}

} // namespace tinylog::bench
