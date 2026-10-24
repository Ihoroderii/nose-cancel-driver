#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
DRIVER_SOURCE="${REPO_ROOT}/build/CleanMic.driver"
DRIVER_TARGET="/Library/Audio/Plug-Ins/HAL/CleanMic.driver"

if [[ ! -d "$DRIVER_SOURCE" ]]; then
    echo "Missing driver bundle at: $DRIVER_SOURCE"
    echo "Build it first with: cmake -S . -B build && cmake --build build"
    exit 1
fi

echo "Installing CleanMic.driver to $DRIVER_TARGET"
sudo rm -rf "$DRIVER_TARGET"
sudo mkdir -p "/Library/Audio/Plug-Ins/HAL"
sudo cp -R "$DRIVER_SOURCE" "$DRIVER_TARGET"

echo "Restarting coreaudiod"
sudo killall coreaudiod || true

echo "CleanMic.driver installed."
echo "Next:"
echo "  1. Run ./build/cleanmic_virtual_host"
echo "  2. Open Teams"
echo "  3. Select CleanMic as the microphone"
