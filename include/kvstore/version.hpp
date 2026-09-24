#pragma once

#include <string_view>

namespace kvstore {

[[nodiscard]] std::string_view version() noexcept;

}  // namespace kvstore
