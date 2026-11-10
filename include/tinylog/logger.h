#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

namespace tinylog {

    enum class Level {
        Debug,
        Info,
        Warn,
        Error
    };

    class Logger {
    public:
        explicit Logger(const std::filesystem::path& file_path);

        void log(Level level, std::string_view message);

        void flush();

    private:
        std::ofstream file_;
    };

} // namespace tinylog
