#!/usr/bin/env bash
set -euo pipefail

SOURCE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/opt/wsl-mesa}"
JOBS="${ASB_MESA_BUILD_JOBS:-$(nproc)}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]]

sudo apt-get update
sudo apt-get build-dep -y mesa
sudo apt-get install -y meson ninja-build directx-headers-dev \
    llvm-21-dev libclc-21 libclc-21-dev llvm-spirv-21 \
    libllvmspirvlib-21-dev libclang-21-dev

cd "$SOURCE"
meson setup --wipe --prefix="$PREFIX" --buildtype=release --strip \
    -D gallium-drivers=llvmpipe,d3d12 \
    -D vulkan-drivers=swrast,microsoft-experimental \
    -D microsoft-clc=enabled \
    -D video-codecs=all \
    -D glvnd=enabled \
    -D platforms=x11,wayland \
    build/
ninja -C build -j "$JOBS"
sudo ninja -C build install
