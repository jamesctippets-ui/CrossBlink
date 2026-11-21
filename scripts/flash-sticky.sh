#!/usr/bin/env bash
# flash-sticky.sh: flash CrossBlink firmware to a Seeed reTerminal Sticky over USB from a Mac.
#
# What it writes:
#   0x10000  firmware.bin  -> app0, the first app slot
#   0xe000   8 KB of 0xFF  -> otadata, blanked so the bootloader starts app0
# Nothing else is touched: no full-chip erase, the bootloader, partition table and NVS
# stay as they are, and the SD card (settings, books, caches) is never written.
#
# Why otadata is blanked: SD Card Firmware Update alternates between app0 and app1 and
# records the active slot in otadata. After an SD update the device may be booting app1,
# so writing app0 alone would leave the old firmware running. A blank otadata makes the
# bootloader fall back to app0, the same thing PlatformIO's boot_app0.bin does on upload.
#
# Usage (Terminal):
#   bash <(curl -fsSL https://raw.githubusercontent.com/jamesctippets-ui/crossblink/main/scripts/flash-sticky.sh)
#   ./scripts/flash-sticky.sh                 # opens Sticky Hub, a local web page (flash + study pack)
#   ./scripts/flash-sticky.sh --main          # flash the latest build from main, in the Terminal
#   ./scripts/flash-sticky.sh --pr 4          # latest build from pull request #4
#   ./scripts/flash-sticky.sh --file ~/Downloads/firmware.bin
#   ./scripts/flash-sticky.sh --help
#
# Works with the macOS system bash (3.2).

set -euo pipefail

REPO="${CROSSBLINK_REPO:-jamesctippets-ui/crossblink}"
# Branch the web page script is downloaded from when this script is not run from a clone.
RAW_REF="${CROSSBLINK_REF:-main}"
# Set by Sticky Hub when it runs this script in the background: never stop to ask anything.
NONINTERACTIVE="${CROSSBLINK_NONINTERACTIVE:-0}"
TOOL_HOME="${CROSSBLINK_FLASHER_HOME:-$HOME/.crossblink-flasher}"
ESPTOOL_VERSION="4.8.1"
CHIP="esp32s3"
APP_OFFSET="0x10000"
OTADATA_OFFSET="0xe000"
OTADATA_SIZE=8192
APP_SLOT_SIZE=6553600 # 0x640000, app0 size in partitions.csv
ESP32S3_CHIP_ID=9     # chip id field in the ESP app image header
BAUD=921600

SOURCE=""
PR_NUMBER=""
FIRMWARE_FILE=""
PORT=""
ASSUME_YES=0
DRY_RUN=0

if [ -t 1 ]; then
  BOLD=$'\033[1m'
  RED=$'\033[31m'
  GREEN=$'\033[32m'
  YELLOW=$'\033[33m'
  RESET=$'\033[0m'
else
  BOLD="" RED="" GREEN="" YELLOW="" RESET=""
fi

say() { printf '%s\n' "$*"; }
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RESET"; }
warn() { printf '%sWarning:%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die() {
  printf '\n%sError:%s %s\n' "$RED" "$RESET" "$*" >&2
  exit 1
}

usage() {
  cat <<EOF
Flash CrossBlink firmware to a Seeed reTerminal Sticky over USB.

Your SD card and settings are not touched. Only the app firmware is replaced.

With no options it opens Sticky Hub, a local web page with a Flash button and a study pack
uploader. Any of the options below runs the flasher in the Terminal instead.

Options:
  --web           Open Sticky Hub (the default when no options are given).
  --main          Use the latest Sticky build from the main branch.
  --pr NUMBER     Use the latest Sticky build from pull request NUMBER.
  --file PATH     Flash a firmware.bin (or the .zip GitHub gives you) you already downloaded.
  --port PATH     Serial port to use, for example /dev/cu.usbmodem101. Found automatically if left out.
  --yes           Do not ask for confirmation before flashing.
  --dry-run       Do everything except writing to the device.
  -h, --help      Show this help.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --main) SOURCE="main" ;;
    --web) SOURCE="web" ;;
    --pr)
      [ $# -ge 2 ] || die "--pr needs a pull request number, for example --pr 4"
      SOURCE="pr"
      PR_NUMBER="${2#\#}"
      shift
      ;;
    --file)
      [ $# -ge 2 ] || die "--file needs a path"
      SOURCE="file"
      FIRMWARE_FILE="$2"
      shift
      ;;
    --port)
      [ $# -ge 2 ] || die "--port needs a path, for example /dev/cu.usbmodem101"
      PORT="$2"
      shift
      ;;
    --yes | -y) ASSUME_YES=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h | --help)
      usage
      exit 0
      ;;
    *) die "Unknown option: $1 (run with --help to see the options)" ;;
  esac
  shift
done

# No source chosen: the web page when run by hand with no options, otherwise main.
if [ -z "$SOURCE" ]; then
  if [ "$ASSUME_YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ] && [ -z "$PORT" ]; then
    SOURCE="web"
  else
    SOURCE="main"
  fi
fi

if [ "$SOURCE" = "pr" ]; then
  case "$PR_NUMBER" in
    '' | *[!0-9]*) die "--pr needs a number, for example --pr 4" ;;
  esac
fi

# Prompts read from the terminal, so they still work when the script itself is piped in.
ask() {
  local prompt="$1" reply=""
  if [ "$NONINTERACTIVE" = "1" ]; then
    printf '%s' "$reply"
    return
  fi
  if [ -r /dev/tty ]; then
    printf '%s' "$prompt" >/dev/tty
    IFS= read -r reply </dev/tty || reply=""
  fi
  printf '%s' "$reply"
}

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/crossblink-flash.XXXXXX")"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

if [ "$(uname -s)" != "Darwin" ]; then
  warn "This script is written for macOS. Continuing anyway."
fi

# --- 1. Tools --------------------------------------------------------------------------------

ensure_python() {
  if command -v python3 >/dev/null 2>&1 && python3 -c 'import venv, ensurepip' >/dev/null 2>&1; then
    return
  fi
  if [ "$(uname -s)" = "Darwin" ]; then
    say "Python 3 is needed. macOS will now offer to install Apple's Command Line Tools."
    say "Click Install, wait for it to finish, then run this script again."
    xcode-select --install >/dev/null 2>&1 || true
    exit 1
  fi
  die "Python 3 with venv is needed. Install it and run this script again."
}

ESPTOOL_PY=""
ensure_esptool() {
  step "Checking esptool"
  ensure_python
  local venv="$TOOL_HOME/venv"
  if [ ! -x "$venv/bin/python" ]; then
    say "Setting up a private Python environment in $venv (one time only)..."
    mkdir -p "$TOOL_HOME"
    python3 -m venv "$venv" || die "Could not create a Python environment in $venv."
  fi
  if ! "$venv/bin/python" -m esptool version 2>/dev/null | grep -q "$ESPTOOL_VERSION"; then
    say "Installing esptool $ESPTOOL_VERSION..."
    "$venv/bin/python" -m pip install --quiet --disable-pip-version-check "esptool==$ESPTOOL_VERSION" ||
      die "Could not install esptool. Check your internet connection and try again."
  fi
  ESPTOOL_PY="$venv/bin/python"
  say "esptool $ESPTOOL_VERSION is ready."
}

ensure_gh() {
  if [ "$NONINTERACTIVE" = "1" ]; then
    command -v gh >/dev/null 2>&1 && gh auth status >/dev/null 2>&1 && return
    die "The GitHub CLI is not installed or not signed in. Run 'gh auth login' in Terminal (or restart Sticky Hub, which sets it up), or flash a file you downloaded."
  fi
  if ! command -v gh >/dev/null 2>&1; then
    say "Downloading builds from GitHub uses the GitHub CLI (gh), which is not installed."
    if command -v brew >/dev/null 2>&1; then
      local reply
      reply="$(ask "Install it now with Homebrew? [Y/n] ")"
      case "$reply" in
        [nN]*) die "gh is needed to download builds. You can pass --file with a firmware.bin instead." ;;
      esac
      brew install gh || die "Homebrew could not install gh."
    else
      die "Install the GitHub CLI from https://cli.github.com (or Homebrew from https://brew.sh, then 'brew install gh'), then run this script again. You can also pass --file with a firmware.bin you downloaded."
    fi
  fi
  if ! gh auth status >/dev/null 2>&1; then
    say "GitHub only lets signed-in users download build artifacts, even for public repositories."
    say "Signing in to GitHub (choose GitHub.com, then 'Login with a web browser')..."
    gh auth login </dev/tty || die "GitHub sign-in did not finish."
  fi
}

# --- 2. Firmware -----------------------------------------------------------------------------

download_artifact() {
  local name="$1" query info id run_id created branch sha
  step "Finding the latest '$name' build on GitHub"
  ensure_gh
  query='[.artifacts[] | select(.expired | not)] | sort_by(.created_at) | reverse | .[0] // empty | [.id, .workflow_run.id, .created_at, .workflow_run.head_branch, .workflow_run.head_sha[0:7]] | @tsv'
  info="$(gh api "repos/$REPO/actions/artifacts?name=$name&per_page=100" --jq "$query")" ||
    die "Could not list builds on GitHub for $REPO."
  if [ -z "$info" ]; then
    if [ "$SOURCE" = "pr" ]; then
      die "No Sticky build found for pull request #$PR_NUMBER. Its CI may still be running, may have failed, or the build expired (they are kept for 14 days). Check https://github.com/$REPO/pull/$PR_NUMBER/checks"
    fi
    die "No Sticky build found for main yet. One is made after each merge to main, or you can start one at https://github.com/$REPO/actions/workflows/sticky-firmware.yml (Run workflow)."
  fi
  IFS=$'\t' read -r id run_id created branch sha <<<"$info"
  say "Build from $branch at commit $sha, made $created"
  say "Run: https://github.com/$REPO/actions/runs/$run_id"
  gh api "repos/$REPO/actions/artifacts/$id/zip" >"$WORK_DIR/artifact.zip" ||
    die "Downloading the build failed."
  FIRMWARE_FILE="$WORK_DIR/artifact.zip"
}

# Accept either a firmware.bin or the .zip that GitHub wraps artifacts in.
unpack_if_zip() {
  case "$FIRMWARE_FILE" in
    *.zip)
      mkdir -p "$WORK_DIR/unzipped"
      unzip -q -o "$FIRMWARE_FILE" -d "$WORK_DIR/unzipped" || die "Could not unzip $FIRMWARE_FILE."
      local found
      found="$(find "$WORK_DIR/unzipped" -type f -name '*.bin' | head -n 1)"
      [ -n "$found" ] || die "No .bin file inside $FIRMWARE_FILE."
      FIRMWARE_FILE="$found"
      ;;
  esac
}

file_size() { wc -c <"$1" | tr -d ' '; }
byte_at() { od -An -tu1 -j "$2" -N 1 "$1" | tr -d ' \n'; }

check_firmware() {
  local f="$1" size magic chip_lo chip_hi chip
  step "Checking the firmware file"
  [ -f "$f" ] || die "File not found: $f"
  size="$(file_size "$f")"
  [ "$size" -gt 1024 ] || die "$f is too small to be firmware ($size bytes)."
  [ "$size" -le "$APP_SLOT_SIZE" ] || die "$f is $size bytes, larger than the app slot ($APP_SLOT_SIZE bytes)."
  magic="$(byte_at "$f" 0)"
  [ "$magic" = "233" ] || die "$f is not an ESP32 app image (it should start with byte 0xE9)."
  chip_lo="$(byte_at "$f" 12)"
  chip_hi="$(byte_at "$f" 13)"
  chip=$((chip_lo + chip_hi * 256))
  if [ "$chip" -ne "$ESP32S3_CHIP_ID" ]; then
    die "$f was built for a different chip (id $chip), not the Sticky's ESP32-S3. Make sure you picked the Sticky build."
  fi
  say "$(basename "$f"): $size bytes, built for ESP32-S3."
}

# --- 3. Device -------------------------------------------------------------------------------

find_port() {
  step "Looking for the Sticky on USB"
  if [ -n "$PORT" ]; then
    [ -e "$PORT" ] || die "Port $PORT does not exist."
    say "Using $PORT"
    return
  fi
  local ports=() p attempt
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    ports=()
    for p in /dev/cu.usbmodem* /dev/ttyACM*; do
      [ -e "$p" ] && ports+=("$p")
    done
    [ ${#ports[@]} -gt 0 ] && break
    if [ "$attempt" -eq 1 ]; then
      say "No device found yet. Plug the Sticky in with a USB-C data cable and wake it up (press a button)."
      say "Waiting up to 30 seconds..."
    fi
    sleep 3
  done
  if [ ${#ports[@]} -eq 0 ]; then
    die "No USB serial device found. Check that the cable carries data (some are charge-only), try another port, and wake the Sticky. If it still does not show up, run with --port /dev/cu.<name> from 'ls /dev/cu.*'."
  fi
  if [ ${#ports[@]} -eq 1 ]; then
    PORT="${ports[0]}"
    say "Found $PORT"
    return
  fi
  say "Found more than one USB serial device:"
  local i=1
  for p in "${ports[@]}"; do
    say "  $i) $p"
    i=$((i + 1))
  done
  local choice
  choice="$(ask "Which one is the Sticky? [1-${#ports[@]}] ")"
  case "$choice" in
    '' | *[!0-9]*) die "No device chosen." ;;
  esac
  [ "$choice" -ge 1 ] && [ "$choice" -le ${#ports[@]} ] || die "No device chosen."
  PORT="${ports[$((choice - 1))]}"
}

# --- Main ------------------------------------------------------------------------------------

# --- Web page mode ---------------------------------------------------------------------------

# Find this script and sticky_hub.py next to it (a clone), or download both from GitHub
# (the curl one-liner, where this script has no file of its own).
start_web() {
  local self_dir="" script_path hub_path
  case "${BASH_SOURCE[0]:-}" in
    /dev/fd/* | /proc/* | "") ;;
    *) self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" ;;
  esac
  if [ -n "$self_dir" ] && [ -f "$self_dir/sticky_hub.py" ]; then
    script_path="$self_dir/$(basename "${BASH_SOURCE[0]}")"
    hub_path="$self_dir/sticky_hub.py"
  else
    step "Downloading Sticky Hub"
    mkdir -p "$TOOL_HOME/bin"
    local raw="https://raw.githubusercontent.com/$REPO/$RAW_REF/scripts"
    if ! curl -fsSL "$raw/flash-sticky.sh" -o "$TOOL_HOME/bin/flash-sticky.sh" ||
      ! curl -fsSL "$raw/sticky_hub.py" -o "$TOOL_HOME/bin/sticky_hub.py"; then
      die "Could not download Sticky Hub from $raw. Check your internet connection."
    fi
    chmod +x "$TOOL_HOME/bin/flash-sticky.sh"
    script_path="$TOOL_HOME/bin/flash-sticky.sh"
    hub_path="$TOOL_HOME/bin/sticky_hub.py"
  fi

  ensure_esptool
  # Set up GitHub downloads now, while there is a Terminal to sign in from. The page still
  # works without it for flashing a downloaded file and for the study pack.
  if ! (ensure_gh); then
    warn "GitHub downloads are not set up. You can still flash a file you downloaded and send study packs."
  fi

  step "Starting Sticky Hub"
  CROSSBLINK_FLASHER_HOME="$TOOL_HOME" CROSSBLINK_REPO="$REPO" \
    "$ESPTOOL_PY" "$hub_path" --script "$script_path" --repo "$REPO"
}

if [ "$SOURCE" = "web" ]; then
  start_web
  exit 0
fi

# --- Terminal mode ---------------------------------------------------------------------------

say "${BOLD}CrossBlink USB flasher for the Seeed reTerminal Sticky${RESET}"
say "Replaces only the app firmware. Your SD card, books and settings are not touched."

case "$SOURCE" in
  main) download_artifact "sticky-firmware-main" ;;
  pr) download_artifact "sticky-firmware-pr$PR_NUMBER" ;;
  file) [ -f "$FIRMWARE_FILE" ] || die "File not found: $FIRMWARE_FILE" ;;
esac
unpack_if_zip
check_firmware "$FIRMWARE_FILE"

ensure_esptool
find_port

head -c "$OTADATA_SIZE" /dev/zero | LC_ALL=C tr '\000' '\377' >"$WORK_DIR/otadata_blank.bin"

FLASH_ARGS=(--chip "$CHIP" --port "$PORT" --baud "$BAUD" --before default_reset --after hard_reset
  write_flash "$OTADATA_OFFSET" "$WORK_DIR/otadata_blank.bin" "$APP_OFFSET" "$FIRMWARE_FILE")

step "Ready to flash"
say "Device:   $PORT"
say "Firmware: $FIRMWARE_FILE"
say "Writes:   app firmware at $APP_OFFSET and the boot slot selector at $OTADATA_OFFSET only"

if [ "$DRY_RUN" -eq 1 ]; then
  say ""
  say "Dry run, nothing written. The command would be:"
  say "  esptool ${FLASH_ARGS[*]}"
  exit 0
fi

if [ "$ASSUME_YES" -ne 1 ]; then
  reply="$(ask "Flash now? Keep the cable plugged in until it finishes. [Y/n] ")"
  case "$reply" in
    [nN]*) die "Cancelled. Nothing was written." ;;
  esac
fi

step "Flashing (about a minute)"
if ! "$ESPTOOL_PY" -m esptool "${FLASH_ARGS[@]}"; then
  say ""
  say "Flashing failed. Things to try:"
  say "  - Unplug and replug the Sticky, wake it, and run the script again."
  say "  - Close anything else using the port (a serial monitor, Arduino IDE, another flasher)."
  say "  - If it still cannot connect, put the Sticky in download mode (hold its BOOT button, if it"
  say "    has one, while plugging in USB or pressing reset), then run the script again."
  say "The SD card was not touched, and the old firmware stays in place if nothing was written."
  exit 1
fi

say ""
say "${GREEN}Done.${RESET} The Sticky is restarting with the new firmware."
say "Check Settings > System to confirm the version."
