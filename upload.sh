#!/usr/bin/env bash
set -e

PORT="${1:-/dev/ttyUSB0}"
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SKETCH_DIR/build"
FQBN="esp32:esp32:esp32da"

# Detect arduino-cli binary
if command -v arduino-cli &>/dev/null; then
    ARDUINO_CLI="arduino-cli"
elif [ -x "$HOME/bin/arduino-cli" ]; then
    ARDUINO_CLI="$HOME/bin/arduino-cli"
elif [ -x "$HOME/.local/bin/arduino-cli" ]; then
    ARDUINO_CLI="$HOME/.local/bin/arduino-cli"
else
    echo "Error: arduino-cli not found in PATH, ~/bin, or ~/.local/bin"
    exit 1
fi

echo "=== Uploading Heat Exchanger Controller to $PORT ==="
if [ -f "$BUILD_DIR/heat_exchanger_controller.ino.bin" ]; then
    echo "Flashing binary from: $BUILD_DIR/heat_exchanger_controller.ino.bin"
    "$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" -i "$BUILD_DIR/heat_exchanger_controller.ino.bin" "$SKETCH_DIR"
else
    "$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
fi
echo "=== Upload Succeeded ==="
