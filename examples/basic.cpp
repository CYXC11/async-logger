#include "tinylog/logger.h"

int main() {
    tinylog::Logger logger("demo.log");

    logger.log(tinylog::Level::Info, "Hello, Logger!");
    logger.log(tinylog::Level::Warn, "This is a warning");
}
