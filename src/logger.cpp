#include "tinylog/logger.h"

#include <stdexcept>

namespace tinylog {

    namespace {

        std::string_view level_name(Level level) {
            switch (level) {
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
            }

            return "UNKNOWN";
        }

    } // namespace

    Logger::Logger(const std::filesystem::path& file_path)
        : file_(file_path, std::ios::app) {

        if (!file_) {
            throw std::runtime_error(
                "Cannot open log file: " + file_path.string()
            );
        }
    }

    void Logger::log(Level level, std::string_view message) {
        file_ << '[' << level_name(level) << "] "
              << message << '\n';

        if (!file_) {
            throw std::runtime_error("Cannot write log message");
        }
    }

} // namespace tinylog
