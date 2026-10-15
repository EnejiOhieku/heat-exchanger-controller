#!/usr/bin/env bash
set -e

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

PORT="${1:-/dev/ttyUSB0}"
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:esp32da"

echo "=== Uploading Heat Exchanger Controller to $PORT ==="
"$ARDUINO_CLI" upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"
echo "=== Upload Succeeded ==="
