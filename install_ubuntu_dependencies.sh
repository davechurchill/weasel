#!/usr/bin/env bash
# Install Weasel's Ubuntu dependencies without vcpkg.
#
# This installs packaged dependencies with APT, then builds the SFML 3 and
# ImGui-SFML versions required by the project from their official sources.

set -euo pipefail

# ---- Config (override by exporting before running) ---------------------------
PREFIX="${PREFIX:-/usr/local}"                 # where SFML and ImGui-SFML install
SRC_PARENT="${SRC_PARENT:-$HOME/src}"          # where third-party source trees live
JOBS="${JOBS:-$(nproc || echo 4)}"             # parallel build jobs

SFML_INSTALL_SCRIPT="${SFML_INSTALL_SCRIPT:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/install_sfml3_ubuntu.sh}"

# Use the released ImGui-SFML 3.0 / Dear ImGui 1.91 pair. Weasel also supports
# its Windows ImGui 1.92 build, but native installs should use released sources.
IMGUI_VERSION="${IMGUI_VERSION:-v1.91.9b}"
IMGUI_REPO_URL="${IMGUI_REPO_URL:-https://github.com/ocornut/imgui.git}"
IMGUI_SFML_REPO_URL="${IMGUI_SFML_REPO_URL:-https://github.com/SFML/imgui-sfml.git}"
IMGUI_SFML_VERSION="${IMGUI_SFML_VERSION:-v3.0}"
# ------------------------------------------------------------------------------

echo "==> Installing Ubuntu packages (sudo)..."
sudo apt update
sudo apt install -y \
  build-essential cmake git pkg-config ninja-build zlib1g-dev \
  libopencv-dev nlohmann-json3-dev \
  ffmpeg zenity

if [ ! -f "$SFML_INSTALL_SCRIPT" ]; then
  echo "SFML installer not found: $SFML_INSTALL_SCRIPT" >&2
  exit 1
fi

echo "==> Installing SFML 3..."
PREFIX="$PREFIX" SRC_PARENT="$SRC_PARENT" JOBS="$JOBS" bash "$SFML_INSTALL_SCRIPT"

mkdir -p "$SRC_PARENT"

IMGUI_DIR="$SRC_PARENT/imgui-${IMGUI_VERSION#v}"
if [ -d "$IMGUI_DIR/.git" ]; then
  echo "==> Found existing Dear ImGui source. Reusing..."
  git -C "$IMGUI_DIR" fetch --tags
  git -C "$IMGUI_DIR" checkout --detach "$IMGUI_VERSION"
else
  echo "==> Cloning Dear ImGui $IMGUI_VERSION..."
  git clone --branch "$IMGUI_VERSION" --depth 1 "$IMGUI_REPO_URL" "$IMGUI_DIR"
fi

IMGUI_SFML_DIR="$SRC_PARENT/imgui-sfml-${IMGUI_SFML_VERSION#v}"
if [ -d "$IMGUI_SFML_DIR/.git" ]; then
    echo "==> Found existing ImGui-SFML source. Reusing..."
    git -C "$IMGUI_SFML_DIR" fetch --tags
    git -C "$IMGUI_SFML_DIR" checkout --detach "$IMGUI_SFML_VERSION"
else
    echo "==> Cloning ImGui-SFML $IMGUI_SFML_VERSION..."
    git clone --branch "$IMGUI_SFML_VERSION" --depth 1 "$IMGUI_SFML_REPO_URL" "$IMGUI_SFML_DIR"
fi

echo "==> Configuring ImGui-SFML (shared)..."
cmake -S "$IMGUI_SFML_DIR" -B "$IMGUI_SFML_DIR/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DIMGUI_DIR="$IMGUI_DIR" \
  -DIMGUI_SFML_FIND_SFML=ON \
  -DIMGUI_SFML_BUILD_EXAMPLES=OFF \
  -DIMGUI_SFML_BUILD_TESTING=OFF

echo "==> Building ImGui-SFML..."
cmake --build "$IMGUI_SFML_DIR/build" --parallel "$JOBS"

echo "==> Installing ImGui-SFML (sudo) to $PREFIX..."
sudo cmake --install "$IMGUI_SFML_DIR/build"
sudo ldconfig

cat <<EOF

==> Dependencies installed.

Build Weasel with CMake. If PREFIX is not /usr/local, include it in
CMAKE_PREFIX_PATH, for example:

  cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release \\
    -DCMAKE_PREFIX_PATH="$PREFIX"
  cmake --build build/native --parallel "$JOBS"

FFmpeg and ffprobe were installed from APT. For a portable Release folder,
copy their executable files into Release/ffmpeg/ after building.
EOF
