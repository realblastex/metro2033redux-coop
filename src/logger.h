#pragma once

// Common logging utilities
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace metro_coop {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    void init(const std::string& log_file = "metro_coop.log");
    void log(const std::string& level, const std::string& message);

    void debug(const std::string& message) {
        if (current_level_ <= LogLevel::Debug) log("DEBUG", message);
    }
    void info(const std::string& message) {
        if (current_level_ <= LogLevel::Info) log("INFO", message);
    }
    void warn(const std::string& message) {
        if (current_level_ <= LogLevel::Warn) log("WARN", message);
    }
    void error(const std::string& message) {
        if (current_level_ <= LogLevel::Error) log("ERROR", message);
    }

    void set_level(LogLevel level) { current_level_ = level; }

private:
    Logger() = default;
    ~Logger() { if (file_.is_open()) file_.close(); }

    std::ofstream file_;
    std::mutex mutex_;
    bool initialized_ = false;
    LogLevel current_level_ = LogLevel::Info; // Default to Info, not Debug
};

// Macro for easy logging
#define LOG_DEBUG(msg) metro_coop::Logger::instance().debug(msg)
#define LOG_INFO(msg) metro_coop::Logger::instance().info(msg)
#define LOG_WARN(msg) metro_coop::Logger::instance().warn(msg)
#define LOG_ERROR(msg) metro_coop::Logger::instance().error(msg)

} // namespace metro_coop