#!/bin/zsh
set -euo pipefail

DRIVER_TARGET="/Library/Audio/Plug-Ins/HAL/CleanMic.driver"

if [[ ! -d "$DRIVER_TARGET" ]]; then
    echo "CleanMic.driver is not installed."
    exit 0
fi

echo "Removing $DRIVER_TARGET"
sudo rm -rf "$DRIVER_TARGET"

echo "Restarting coreaudiod"
sudo killall coreaudiod || true

echo "CleanMic.driver removed."
