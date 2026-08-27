#!/usr/bin/env bash
# Build + install SFML 3 as shared libraries on Ubuntu.

set -euo pipefail

# ---- Config (override by exporting before running) ---------------------------
SFML_VERSION="${SFML_VERSION:-3.0.1}"          # e.g. 3.0.1
PREFIX="${PREFIX:-/usr/local}"                 # install prefix
SRC_PARENT="${SRC_PARENT:-$HOME/src}"          # where to put sources
REPO_URL="${REPO_URL:-https://github.com/SFML/SFML.git}"
JOBS="${JOBS:-$(nproc || echo 4)}"             # parallel build jobs
# ------------------------------------------------------------------------------

echo "==> Using:"
echo "    SFML_VERSION = $SFML_VERSION"
echo "    PREFIX       = $PREFIX"
echo "    SRC_PARENT   = $SRC_PARENT"
echo "    JOBS         = $JOBS"
echo

# 0) Prerequisites
echo "==> Installing build tools and SFML dependencies (sudo)..."
sudo apt update
sudo apt install -y \
  build-essential cmake git pkg-config \
  libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev \
  libudev-dev libgl1-mesa-dev libopengl-dev \
  libopenal-dev libflac-dev libvorbis-dev \
  libfreetype6-dev \
  libjpeg-dev libpng-dev

# 1) Get source
mkdir -p "$SRC_PARENT"
cd "$SRC_PARENT"

if [ -d "sfml-3" ]; then
  echo "==> Found existing sfml-3 directory. Reusing..."
  cd sfml-3
  git fetch --tags
  git checkout "tags/$SFML_VERSION" -f
else
  echo "==> Cloning SFML $SFML_VERSION..."
  git clone --branch "$SFML_VERSION" --depth 1 "$REPO_URL" sfml-3
  cd sfml-3
fi

# 2) Fresh configure for shared libraries
echo "==> Configuring (BUILD_SHARED_LIBS=ON) ..."
rm -rf build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"

# 3) Build
echo "==> Building..."
cmake --build build --parallel "$JOBS"

# 4) Install
echo "==> Installing (sudo) to $PREFIX ..."
sudo cmake --install build

# Save install manifest for easy uninstall later
MANIFEST_DST="$PREFIX/share/sfml3-install-manifest-${SFML_VERSION}.txt"
echo "==> Saving install manifest to: $MANIFEST_DST (sudo)"
sudo mkdir -p "$(dirname "$MANIFEST_DST")"
sudo cp build/install_manifest.txt "$MANIFEST_DST"

# Refresh dynamic linker cache
echo "==> Running ldconfig (sudo)..."
sudo ldconfig
