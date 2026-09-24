#pragma once

#include <mutex>
#include <optional>
#include <string_view>

namespace kvstore::util {

enum class LogLevel { kDebug, kInfo, kWarn, kError };

// Minimal thread-safe stderr logger. SPEC.md section 8 requires startup
// configuration, bind/listen failure, malformed requests, resource-limit
// failures, shutdown start, and shutdown completion to be logged; this is
// intentionally small (no rotation, no structured fields, no async queue)
// since nothing beyond those events is specified.
//
// Logs must never include complete client payload values (SPEC.md section
// 8) -- callers are responsible for keeping messages to metadata (event
// names, counts, addresses), never raw keys/values.
class Logger {
 public:
  explicit Logger(LogLevel min_level = LogLevel::kInfo) : min_level_(min_level) {}

  void log(LogLevel level, std::string_view message);

  void debug(std::string_view message) { log(LogLevel::kDebug, message); }
  void info(std::string_view message) { log(LogLevel::kInfo, message); }
  void warn(std::string_view message) { log(LogLevel::kWarn, message); }
  void error(std::string_view message) { log(LogLevel::kError, message); }

  // Parses a CLI --log-level value ("debug", "info", "warn", "error",
  // case-insensitive). Returns std::nullopt if `name` does not match one.
  [[nodiscard]] static std::optional<LogLevel> parse_level(std::string_view name);

 private:
  LogLevel min_level_;
  std::mutex mutex_;
};

}  // namespace kvstore::util
