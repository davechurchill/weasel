#!/usr/bin/env sh
set -eu

PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD_DIR="$PROJECT_DIR/build/cmake-native"
CONFIG=${1:-Release}

# Ubuntu installs SFML 3 and ImGui-SFML into /usr/local by default. Homebrew
# usually lives under /opt/homebrew or /usr/local, so include its prefix too.
PREFIX_PATH=${WEASEL_CMAKE_PREFIX_PATH:-/usr/local}
if command -v brew >/dev/null 2>&1; then
    PREFIX_PATH="$(brew --prefix);$PREFIX_PATH"
fi

cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    "-DCMAKE_PREFIX_PATH=$PREFIX_PATH"
cmake --build "$BUILD_DIR" --parallel
