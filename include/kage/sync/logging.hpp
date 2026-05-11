#pragma once

#include <cstdint>
#include <memory>

namespace spdlog {
class logger;
}

namespace kage::sync {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

enum class LogFormat {
    Text,
    Json
};

struct LoggingOptions {
    LogLevel level = LogLevel::Off;
    LogFormat format = LogFormat::Text;
    std::uint32_t max_warning_logs_per_source = 64;
    std::shared_ptr<spdlog::logger> logger;
};

}  // namespace kage::sync
