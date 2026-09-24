#include "kvstore/util/logger.hpp"

#include <array>
#include <cstddef>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>

namespace kvstore::util {

namespace {

[[nodiscard]] std::string_view level_name(LogLevel level) {
  switch (level) {
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
  }
  return "UNKNOWN";
}

// "YYYY-MM-DDTHH:MM:SSZ", UTC.
[[nodiscard]] std::string timestamp_now() {
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  ::gmtime_r(&now, &utc);
  std::array<char, 32> buf{};
  const std::size_t len = std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buf.data(), len);
}

}  // namespace

void Logger::log(LogLevel level, std::string_view message) {
  if (level < min_level_) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::cerr << "[" << timestamp_now() << "] [" << level_name(level) << "] " << message << '\n';
}

std::optional<LogLevel> Logger::parse_level(std::string_view name) {
  std::string lowered(name);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  if (lowered == "debug") {
    return LogLevel::kDebug;
  }
  if (lowered == "info") {
    return LogLevel::kInfo;
  }
  if (lowered == "warn" || lowered == "warning") {
    return LogLevel::kWarn;
  }
  if (lowered == "error") {
    return LogLevel::kError;
  }
  return std::nullopt;
}

}  // namespace kvstore::util
