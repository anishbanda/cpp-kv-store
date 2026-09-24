#include <iostream>
#include <string_view>

#include "kvstore/version.hpp"

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view{argv[1]} == "--version") {
    std::cout << "cpp-kv-store " << kvstore::version() << '\n';
    return 0;
  }

  std::cout << "cpp-kv-store scaffold is ready. Implement Milestone 1 next.\n";
  return 0;
}
