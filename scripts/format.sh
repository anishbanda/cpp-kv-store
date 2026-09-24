#!/usr/bin/env bash
set -euo pipefail

mapfile -t files < <(find include src tests benchmarks -type f \
  \( -name '*.cpp' -o -name '*.hpp' -o -name '*.cc' -o -name '*.hh' \) | sort)

if ((${#files[@]} == 0)); then
  echo "No C++ files found."
  exit 0
fi

clang-format -i "${files[@]}"

