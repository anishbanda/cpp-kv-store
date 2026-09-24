#include "kvstore/cli/options.hpp"

#include <charconv>
#include <cstddef>
#include <stdexcept>

#include "kvstore/util/logger.hpp"

namespace kvstore::cli {

namespace {

template <typename T>
[[nodiscard]] T parse_unsigned(std::string_view flag, std::string_view value) {
  T result{};
  const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || ptr != value.data() + value.size()) {
    throw std::invalid_argument("invalid value for " + std::string(flag) + ": '" +
                                std::string(value) + "'");
  }
  return result;
}

[[nodiscard]] std::string_view next_value(const std::vector<std::string_view>& args, std::size_t& i,
                                          std::string_view flag) {
  if (i + 1 >= args.size()) {
    throw std::invalid_argument("missing value for " + std::string(flag));
  }
  return args[++i];
}

}  // namespace

std::string usage_text() {
  return "Usage: kvstore_server [options]\n"
         "\n"
         "Options:\n"
         "  --bind <address>        Bind address (default: 0.0.0.0)\n"
         "  --port <port>           Listen port (default: 6380)\n"
         "  --workers <count>       Worker thread count (default: 4)\n"
         "  --shards <count>        Storage shard count, rounded up to a power of two "
         "(default: 32)\n"
         "  --queue-capacity <n>    Work/completion queue capacity (default: 1024)\n"
         "  --log-level <level>     debug | info | warn | error (default: info)\n"
         "  --help                  Print this message and exit\n"
         "  --version               Print the version and exit\n";
}

ServerOptions parse_args(const std::vector<std::string_view>& args) {
  ServerOptions options;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];

    if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--version" || arg == "-v") {
      options.show_version = true;
    } else if (arg == "--bind") {
      options.bind_address = std::string(next_value(args, i, arg));
    } else if (arg == "--port") {
      options.port = parse_unsigned<std::uint16_t>(arg, next_value(args, i, arg));
    } else if (arg == "--workers") {
      options.worker_count = parse_unsigned<std::size_t>(arg, next_value(args, i, arg));
    } else if (arg == "--shards") {
      options.shard_count = parse_unsigned<std::size_t>(arg, next_value(args, i, arg));
    } else if (arg == "--queue-capacity") {
      options.queue_capacity = parse_unsigned<std::size_t>(arg, next_value(args, i, arg));
    } else if (arg == "--log-level") {
      const std::string_view value = next_value(args, i, arg);
      if (!util::Logger::parse_level(value).has_value()) {
        throw std::invalid_argument("invalid value for --log-level: '" + std::string(value) + "'");
      }
      options.log_level = std::string(value);
    } else {
      throw std::invalid_argument("unknown option: '" + std::string(arg) + "'");
    }
  }

  return options;
}

}  // namespace kvstore::cli
