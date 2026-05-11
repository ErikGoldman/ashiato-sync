#pragma once

#include "kage/sync/logging.hpp"

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/common.h>

namespace spdlog {
class logger;
}

namespace kage::sync::detail {

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept;
std::shared_ptr<spdlog::logger> make_logger(const LoggingOptions& options, const char* name);
std::string log_token(std::string_view value);

}  // namespace kage::sync::detail
