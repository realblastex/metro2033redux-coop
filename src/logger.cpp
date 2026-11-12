// Logger implementation
#include "logger.h"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace metro_coop {

void Logger::init(const std::string& log_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;

    file_.open(log_file, std::ios::out | std::ios::app);
    if (file_.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        file_ << "\n=== Metro Co-op Mod Log Started: " << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << " ===\n";
        initialized_ = true;
    }
}

void Logger::log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !file_.is_open()) return;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    file_ << "[" << std::put_time(std::localtime(&time), "%H:%M:%S")
          << "." << std::setfill('0') << std::setw(3) << ms.count()
          << "] [" << level << "] " << message << "\n";
    file_.flush();
}

} // namespace metro_coop