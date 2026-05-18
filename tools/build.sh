#!/usr/bin/env bash
# Build the firmware inside the ESP-IDF dev container for a specific target.
#
# Usage: tools/build.sh <esp32|esp32s3>
#
# The target is required; there is intentionally no default so a build
# always names the chip explicitly. `idf.py set-target` regenerates
# sdkconfig from sdkconfig.defaults + sdkconfig.defaults.<target>, so
# switching back and forth is safe.

set -euo pipefail

TARGET="${1:-}"
case "$TARGET" in
  esp32|esp32s3) ;;
  *)
    echo "Usage: $0 <esp32|esp32s3>" >&2
    exit 1
    ;;
esac

IDF_IMAGE="espressif/idf:v5.5.1"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

exec docker run --rm -t \
  -v "$PROJECT_DIR":/project \
  -w /project \
  "$IDF_IMAGE" \
  bash -c "idf.py set-target $TARGET && idf.py build"
