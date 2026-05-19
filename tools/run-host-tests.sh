#!/usr/bin/env bash
# Compile and run the host-side unit tests (pure C, no IDF needed).
# Useful both interactively and from any CI hook.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR/tests/host"
make --no-print-directory clean
exec make --no-print-directory run
