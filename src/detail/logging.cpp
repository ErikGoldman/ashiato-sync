#include "detail/logging.hpp"

#include <spdlog/fmt/fmt.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string_view>

namespace ashiato::sync::detail {
namespace {

void append_json_string(spdlog::memory_buf_t& dest, std::string_view value) {
    for (const char ch : value) {
        switch (ch) {
        case '"':
            fmt::format_to(std::back_inserter(dest), "\\\"");
            break;
        case '\\':
            fmt::format_to(std::back_inserter(dest), "\\\\");
            break;
        case '\b':
            fmt::format_to(std::back_inserter(dest), "\\b");
            break;
        case '\f':
            fmt::format_to(std::back_inserter(dest), "\\f");
            break;
        case '\n':
            fmt::format_to(std::back_inserter(dest), "\\n");
            break;
        case '\r':
            fmt::format_to(std::back_inserter(dest), "\\r");
            break;
        case '\t':
            fmt::format_to(std::back_inserter(dest), "\\t");
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U) {
                fmt::format_to(std::back_inserter(dest), "\\u{:04x}", static_cast<unsigned int>(ch));
            } else {
                dest.push_back(ch);
            }
            break;
        }
    }
}

bool json_value_is_number(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (value[index] == '-') {
        ++index;
    }
    bool saw_digit = false;
    for (; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
            continue;
        }
        if (ch == '.') {
            continue;
        }
        return false;
    }
    return saw_digit;
}

void append_json_field_value(spdlog::memory_buf_t& dest, std::string_view value) {
    if (value == "true" || value == "false" || json_value_is_number(value)) {
        fmt::format_to(std::back_inserter(dest), "{}", value);
        return;
    }
    dest.push_back('"');
    append_json_string(dest, value);
    dest.push_back('"');
}

void append_payload_fields(spdlog::memory_buf_t& dest, std::string_view payload) {
    std::size_t start = 0;
    while (start < payload.size()) {
        while (start < payload.size() && payload[start] == ' ') {
            ++start;
        }
        const std::size_t end = payload.find(' ', start);
        const std::string_view token = payload.substr(
            start,
            end == std::string_view::npos ? std::string_view::npos : end - start);
        const std::size_t equals = token.find('=');
        if (equals != std::string_view::npos && equals != 0U && equals + 1U < token.size()) {
            fmt::format_to(std::back_inserter(dest), ",\"");
            append_json_string(dest, token.substr(0, equals));
            fmt::format_to(std::back_inserter(dest), "\":");
            append_json_field_value(dest, token.substr(equals + 1U));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
}

class JsonLogFormatter final : public spdlog::formatter {
public:
    void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override {
        const auto level = spdlog::level::to_string_view(msg.level);
        fmt::format_to(std::back_inserter(dest), "{{\"level\":\"");
        append_json_string(dest, std::string_view(level.data(), level.size()));
        fmt::format_to(std::back_inserter(dest), "\",\"logger\":\"");
        append_json_string(dest, std::string_view(msg.logger_name.data(), msg.logger_name.size()));
        dest.push_back('"');
        append_payload_fields(dest, std::string_view(msg.payload.data(), msg.payload.size()));
        fmt::format_to(std::back_inserter(dest), "}}\n");
    }

    std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<JsonLogFormatter>();
    }
};

}  // namespace

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warning:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    case LogLevel::Off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

std::shared_ptr<spdlog::logger> make_logger(const LoggingOptions& options, const char* name) {
    if (options.logger != nullptr) {
        options.logger->set_level(to_spdlog_level(options.level));
        return options.logger;
    }
    if (options.level == LogLevel::Off) {
        return nullptr;
    }

    auto logger = std::make_shared<spdlog::logger>(
        name,
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    logger->set_level(to_spdlog_level(options.level));
    if (options.format == LogFormat::Json) {
        logger->set_formatter(std::make_unique<JsonLogFormatter>());
    } else {
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    }
    return logger;
}

std::string log_token(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        result.push_back(ch == ' ' ? '_' : ch);
    }
    return result;
}

}  // namespace ashiato::sync::detail
