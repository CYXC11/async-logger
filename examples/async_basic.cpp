#include "tinylog/async_logger.h"

#include <string>

int main() {
    tinylog::AsyncLogger logger("async_test.log");

    for (int i = 0; i < 100; ++i) {
        logger.log(
            tinylog::Level::Info,
            "Message " + std::to_string(i)
        );
    }
}