#!/usr/bin/env bash
# Install Weasel's native macOS build dependencies without vcpkg.

set -euo pipefail

# Override these before running if you want a different install/source location.
# By default, install native macOS dependencies into the active Homebrew prefix
# (/opt/homebrew on Apple Silicon, /usr/local on Intel).
PREFIX="${PREFIX:-}"
SRC_PARENT="${SRC_PARENT:-${HOME}/src/weasel-deps}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

# Use only released ImGui-SFML 3.0 / Dear ImGui 1.91 sources. Weasel also
# supports its Windows ImGui 1.92 build, but native installs need not depend
# on an unmerged ImGui-SFML change.
IMGUI_REF="${IMGUI_REF:-v1.91.9b}"
IMGUI_SFML_REF="${IMGUI_SFML_REF:-v3.0}"
IMGUI_REPOSITORY="${IMGUI_REPOSITORY:-https://github.com/ocornut/imgui.git}"
IMGUI_SFML_REPOSITORY="${IMGUI_SFML_REPOSITORY:-https://github.com/SFML/imgui-sfml.git}"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This installer is for macOS only." >&2
    exit 1
fi

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Xcode Command Line Tools are required. Run: xcode-select --install" >&2
    exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required. Install it from https://brew.sh, then rerun this script." >&2
    exit 1
fi

BREW_PREFIX="$(brew --prefix)"
if [ -z "$PREFIX" ]; then
    PREFIX="$BREW_PREFIX"
fi

echo "==> Installing packaged dependencies with Homebrew..."
brew install cmake ninja pkg-config sfml opencv nlohmann-json ffmpeg

if ! command -v git >/dev/null 2>&1; then
    echo "git was not found after checking the Xcode Command Line Tools." >&2
    exit 1
fi

checkout_source() {
    repository="$1"
    source_dir="$2"
    reference="$3"

    if [ ! -e "$source_dir/.git" ]; then
        git clone --no-checkout --depth 1 "$repository" "$source_dir"
    fi

    if ! git -C "$source_dir" fetch --depth 1 origin "$reference"; then
        echo "Could not fetch $reference from $repository." >&2
        echo "Set the matching IMGUI_REF or IMGUI_SFML_REF and run this script again." >&2
        exit 1
    fi

    git -C "$source_dir" checkout --detach FETCH_HEAD
}

mkdir -p "$SRC_PARENT"
IMGUI_DIR="$SRC_PARENT/imgui-${IMGUI_REF#v}"
IMGUI_SFML_DIR="$SRC_PARENT/imgui-sfml-${IMGUI_SFML_REF#v}"

echo "==> Fetching Dear ImGui $IMGUI_REF..."
checkout_source "$IMGUI_REPOSITORY" "$IMGUI_DIR" "$IMGUI_REF"

echo "==> Fetching ImGui-SFML $IMGUI_SFML_REF..."
checkout_source "$IMGUI_SFML_REPOSITORY" "$IMGUI_SFML_DIR" "$IMGUI_SFML_REF"

IMGUI_SFML_BUILD_DIR="$IMGUI_SFML_DIR/build"

echo "==> Building ImGui-SFML (which compiles and installs Dear ImGui with it)..."
cmake -S "$IMGUI_SFML_DIR" -B "$IMGUI_SFML_BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON \
    "-DIMGUI_DIR=$IMGUI_DIR" \
    -DIMGUI_SFML_FIND_SFML=ON \
    "-DCMAKE_PREFIX_PATH=$PREFIX;$BREW_PREFIX"
cmake --build "$IMGUI_SFML_BUILD_DIR" --parallel "$JOBS"

if [ -w "$PREFIX" ]; then
    cmake --install "$IMGUI_SFML_BUILD_DIR"
else
    echo "==> Installing ImGui-SFML into $PREFIX (sudo may prompt)..."
    sudo cmake --install "$IMGUI_SFML_BUILD_DIR"
fi

echo
echo "Done. Build Weasel with: sh scripts/build/build_cmake.sh"
if [ "$PREFIX" != "$BREW_PREFIX" ]; then
    echo "For this custom PREFIX, use:"
    echo "  WEASEL_CMAKE_PREFIX_PATH=\"$PREFIX;$BREW_PREFIX\" sh scripts/build/build_cmake.sh"
fi
