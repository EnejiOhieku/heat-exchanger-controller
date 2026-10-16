#!/usr/bin/env bash
set -e

# Terminal colors
CLR_RESET="\033[0m"
CLR_BOLD="\033[1m"
CLR_RED="\033[1;31m"
CLR_GREEN="\033[1;32m"
CLR_YELLOW="\033[1;33m"
CLR_BLUE="\033[1;34m"
CLR_MAGENTA="\033[1;35m"
CLR_CYAN="\033[1;36m"
CLR_GRAY="\033[0;90m"

log_info()    { echo -e "${CLR_BLUE}[INFO]${CLR_RESET} $1"; }
log_step()    { echo -e "\n${CLR_CYAN}${CLR_BOLD}==> [$1] $2${CLR_RESET}"; }
log_success() { echo -e "${CLR_GREEN}${CLR_BOLD}[✓ SUCCESS]${CLR_RESET} $1"; }
log_warn()    { echo -e "${CLR_YELLOW}[WARN]${CLR_RESET} $1"; }
log_err()     { echo -e "${CLR_RED}${CLR_BOLD}[✗ ERROR]${CLR_RESET} $1"; }

# Failure trap
trap 'on_error $? $LINENO' ERR
on_error() {
    local exit_code=$1
    local line_no=$2
    echo ""
    log_err "Compilation pipeline failed at line $line_no with exit code $exit_code."
    echo -e "${CLR_YELLOW}Tip: Check the compiler diagnostic output above for details.${CLR_RESET}"
    exit "$exit_code"
}

START_TIME=$(date +%s)
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SKETCH_DIR/build"
FQBN="esp32:esp32:esp32da"

mkdir -p "$BUILD_DIR"

echo -e "${CLR_MAGENTA}${CLR_BOLD}======================================================${CLR_RESET}"
echo -e "${CLR_MAGENTA}${CLR_BOLD}   ESP32 Heat Exchanger Controller - Build System     ${CLR_RESET}"
echo -e "${CLR_MAGENTA}${CLR_BOLD}======================================================${CLR_RESET}"
echo -e "Started: $(date '+%Y-%m-%d %H:%M:%S')"
echo -e "Sketch:  ${CLR_BOLD}$SKETCH_DIR${CLR_RESET}"
echo -e "Target:  ${CLR_BOLD}$FQBN (ESP32 DevKitC)${CLR_RESET}"
echo -e "Output:  ${CLR_BOLD}$BUILD_DIR${CLR_RESET}"

# Step 1: Toolchain & Environment Verification
log_step "1/4" "Verifying toolchain and build environment..."
if command -v arduino-cli &>/dev/null; then
    ARDUINO_CLI="arduino-cli"
elif [ -x "$HOME/bin/arduino-cli" ]; then
    ARDUINO_CLI="$HOME/bin/arduino-cli"
elif [ -x "$HOME/.local/bin/arduino-cli" ]; then
    ARDUINO_CLI="$HOME/.local/bin/arduino-cli"
else
    log_err "arduino-cli not found in PATH, ~/bin, or ~/.local/bin."
    exit 1
fi

CLI_VERSION=$("$ARDUINO_CLI" version 2>/dev/null | head -n1)
log_info "CLI Engine:   ${CLR_BOLD}$CLI_VERSION${CLR_RESET} ($ARDUINO_CLI)"

PYTHON_BIN=$(command -v python3 || command -v python || true)
if [ -z "$PYTHON_BIN" ]; then
    log_err "Python 3 is required for bundling web assets but was not found."
    exit 1
fi
log_info "Python:       $($PYTHON_BIN --version 2>&1) ($PYTHON_BIN)"

# Verify ESP32 core
ESP32_CORE=$("$ARDUINO_CLI" core list 2>/dev/null | grep -E "esp32:esp32" || true)
if [ -z "$ESP32_CORE" ]; then
    log_warn "ESP32 platform core not detected. Ensure esp32:esp32 is installed."
else
    log_info "Platform:     $(echo "$ESP32_CORE" | awk '{print $1, $2}')"
fi

# Step 2: Web Assets Bundler
log_step "2/4" "Bundling web assets (HTML, CSS, JS -> C++ Header)..."
BUNDLE_SCRIPT="$SKETCH_DIR/src/web/bundle.py"
if [ -f "$BUNDLE_SCRIPT" ]; then
    "$PYTHON_BIN" "$BUNDLE_SCRIPT"
    GEN_HEADER="$SKETCH_DIR/src/network/web_dashboard.hpp"
    if [ -f "$GEN_HEADER" ]; then
        HEADER_SIZE=$(stat -c%s "$GEN_HEADER" 2>/dev/null || wc -c < "$GEN_HEADER")
        log_info "Asset header generated: ${CLR_BOLD}src/network/web_dashboard.hpp${CLR_RESET} (${HEADER_SIZE} bytes)"
    fi
else
    log_warn "Bundler script not found at $BUNDLE_SCRIPT; skipping web asset bundle."
fi

# Step 3: Compilation
log_step "3/4" "Compiling sketch and library sources to build directory..."
BUILD_CMD=("$ARDUINO_CLI" compile --fqbn "$FQBN" --build-path "$BUILD_DIR" "$SKETCH_DIR")

# Append user flags or extra args
if [ $# -gt 0 ]; then
    log_info "Applying extra build options: $*"
    BUILD_CMD+=("$@")
fi

TMP_LOG=$(mktemp)
"${BUILD_CMD[@]}" 2>&1 | tee "$TMP_LOG"
BUILD_STATUS="${PIPESTATUS[0]}"

if [ "$BUILD_STATUS" -ne 0 ]; then
    rm -f "$TMP_LOG"
    exit "$BUILD_STATUS"
fi

# Step 4: Build Analysis & Memory Report
log_step "4/4" "Analyzing binary footprint and artifacts..."

FLASH_LINE=$(grep -E "Sketch uses|program storage space" "$TMP_LOG" | tail -n1 || true)
RAM_LINE=$(grep -E "Global variables use|dynamic memory" "$TMP_LOG" | tail -n1 || true)
rm -f "$TMP_LOG"

if [ -n "$FLASH_LINE" ]; then
    echo -e "  ${CLR_CYAN}Flash (Program Storage):${CLR_RESET} $FLASH_LINE"
fi
if [ -n "$RAM_LINE" ]; then
    echo -e "  ${CLR_CYAN}RAM   (Dynamic Memory): ${CLR_RESET} $RAM_LINE"
fi

echo -e "\n  ${CLR_BOLD}Generated Artifacts in $BUILD_DIR:${CLR_RESET}"
for f in "$BUILD_DIR"/*.bin "$BUILD_DIR"/*.elf; do
    if [ -f "$f" ]; then
        FNAME=$(basename "$f")
        FSIZE=$(stat -c%s "$f" 2>/dev/null || wc -c < "$f")
        printf "    %-42s %8s bytes\n" "$FNAME" "$FSIZE"
    fi
done

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo -e "${CLR_GREEN}${CLR_BOLD}======================================================${CLR_RESET}"
echo -e "${CLR_GREEN}${CLR_BOLD}   BUILD SUCCESSFUL! (Elapsed: ${ELAPSED}s)            ${CLR_RESET}"
echo -e "${CLR_GREEN}${CLR_BOLD}======================================================${CLR_RESET}"
echo -e "Target Firmware: Ready for upload via ${CLR_BOLD}./upload.sh [PORT]${CLR_RESET}\n"
