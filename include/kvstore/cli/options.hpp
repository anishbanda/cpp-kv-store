#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kvstore::cli {

// SPEC.md section 8: "Version 1 must accept command-line options for:
// bind address, port, worker count, shard count, queue capacity, log
// level." Defaults match SPEC.md's stated defaults where given (bind
// 0.0.0.0, port 6380) and this project's own documented choices
// elsewhere (32 shards -- SPEC.md section 4).
struct ServerOptions {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 6380;
  std::size_t worker_count = 4;
  std::size_t shard_count = 32;
  std::size_t queue_capacity = 1024;
  std::string log_level = "info";

  // Set when --help or --version was given; the caller should print the
  // corresponding text and exit 0 without starting a server.
  bool show_help = false;
  bool show_version = false;
};

[[nodiscard]] std::string usage_text();

// Parses `args` (argv[1..argc), i.e. excluding the program name).
// Throws std::invalid_argument, with a human-readable message, on an
// unknown flag, a missing value, or a value that fails to parse.
[[nodiscard]] ServerOptions parse_args(const std::vector<std::string_view>& args);

}  // namespace kvstore::cli
