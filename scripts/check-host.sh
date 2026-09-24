#!/usr/bin/env bash
set -euo pipefail

failures=0

check_command() {
  local command_name="$1"
  local help_text="$2"

  if command -v "${command_name}" >/dev/null 2>&1; then
    printf 'OK   %s\n' "${command_name}"
  else
    printf 'MISS %s — %s\n' "${command_name}" "${help_text}"
    failures=$((failures + 1))
  fi
}

check_command git "install Apple Command Line Tools with: xcode-select --install"
check_command docker "install and open Docker Desktop"

if command -v docker >/dev/null 2>&1; then
  if docker info >/dev/null 2>&1; then
    printf 'OK   Docker daemon is running\n'
  else
    printf 'MISS Docker is installed but its daemon is unavailable — open Docker Desktop\n'
    failures=$((failures + 1))
  fi

  if docker compose version >/dev/null 2>&1; then
    printf 'OK   Docker Compose\n'
  else
    printf 'MISS Docker Compose — update or reinstall Docker Desktop\n'
    failures=$((failures + 1))
  fi
fi

if ((failures > 0)); then
  printf '\nHost check found %d problem(s). Fix them before building.\n' "${failures}"
  exit 1
fi

printf '\nHost prerequisites look ready. Next run: docker compose build dev\n'

