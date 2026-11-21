---
title: Installation
nav_order: 2
---

# Installation

## Supported Devices

- Xteink X3, X4
- Xteink X4 Classic and X4 Pro
- Seeed Studio Sticky

Don't have a device yet? Get one directly from [Xteink](https://go.sjv.io/X4RGBb) or [Seeed Studio](https://www.seeedstudio.com/reTerminal-Sticky-p-6861.html?sensecap_affiliate=1Nxo3Gw&referring_service=link).

Note: Your purchases using the above affiliate links help support ongoing development of Crossink.

## Web Installation via USB

#### For new installs and updates.

1. Navigate to [https://inky.crossink.dev/#flash-tools](https://inky.crossink.dev/#flash-tools) and select your device model.
2. The latest version will be automatically selected, but if you ever want to revert to an earlier build, you can select it from the dropdown.
3. Choose the firmware option you want to install.
4. Click on the "Flash Firmware" button

X4 Pro uses the ESP32-S3 firmware option. Keep the reader connected during the
download-mode and flashing steps shown by Inky.

## USB Drive

On X4 Pro, choose `Home > File Transfer > USB Drive` to expose the SD card to
your computer. Eject the drive from the computer before disconnecting it; the
reader restarts to Home when the drive is safely ejected or the cable is
removed.

## SD Card Firmware Update

#### For installing newer versions of CrossInk. Can be used by USB locked devices.

1. Follow the same steps from the Web Installation method above. There will be an option to download the firmware instead of USB flashing.
2. Place the downloaded `firmware-*.bin` file on your SD card. You can place this file anywhere.
3. Go to `Settings > System > SD Card Firmware Update` and navigate to the `.bin` file and update.

## USB Locked Devices

If your device has USB data transfer disabled:

1. Navigate to [https://inky.crossink.dev/#flash-tools](https://inky.crossink.dev/#flash-tools) and check the box for "I have a locked device" at the top.
2. The latest version will be automatically selected, but if you ever want to revert to an earlier build, you can select it from the dropdown.
3. Choose the firmware option you want to download.
4. Click on the "Download update.bin" button and follow the instructions.

## CrossBlink: Sticky Hub for macOS (USB flashing and study packs)

Sticky Hub is a local web page on your Mac that flashes the Sticky over USB and
copies a Study-App study pack to it. Only the app firmware is replaced: the SD
card (books, settings, caches, study progress) and the rest of flash are left
alone.

Open Terminal and run:

```sh
bash <(curl -fsSL https://raw.githubusercontent.com/jamesctippets-ui/crossblink/main/scripts/flash-sticky.sh)
```

The first run sets up `esptool` in `~/.crossblink-flasher` and, so builds can be
downloaded, installs the GitHub CLI with Homebrew and asks you to sign in once
(GitHub requires sign-in for build downloads). The page then opens in your
browser. Keep the Terminal window open while you use it; Ctrl+C stops it.

**Flash firmware (USB).** Plug the Sticky in with a USB-C data cable and wake
it. Pick the latest build from `main`, an open pull request's build, or a
`firmware.bin`/artifact `.zip` you downloaded, then click **Flash Sticky**.
Files that are not ESP32-S3 app images, or are too large for the app slot, are
refused.

**Load study pack (Wi-Fi).** On the Sticky open File Transfer, on the same Wi-Fi
as the Mac. Choose the study pack folder (the `study` folder written by
Study-App's `tools/eink_pack.py`, usually `Study-App/eink-out/study`), check the
reader address shown on the Sticky (`crosspoint.local` by default), and click
**Send study pack**. Files go to `/study/` on the SD card. Files in the pack
replace their old copies; `progress.jsonl`, dot-folders such as `.srs`, and
anything not in the pack are left alone. `manifest.json` is sent last.

### Terminal only

Pass any option to flash without the page (add it after the command above, or
run `scripts/flash-sticky.sh` from a clone):

| Option | What it does |
| --- | --- |
| `--main` | Flash the newest Sticky build from `main` |
| `--pr 4` | Flash the newest Sticky build from pull request #4 |
| `--file ~/Downloads/sticky-firmware-pr4.zip` | Flash a `firmware.bin` or artifact `.zip` you already downloaded |
| `--port /dev/cu.usbmodem101` | Use this serial port instead of finding it automatically |
| `--dry-run` | Do every check but write nothing |

Under the hood it writes `firmware.bin` to the first app slot (`0x10000`) and
blanks `otadata` (`0xe000`, 8 KB) so the bootloader starts that slot, since an
earlier SD Card Firmware Update may have left the device booting the second
slot. NVS, the bootloader and the partition table are not written.

## Command Line

These instructions are for macOS and Linux. Windows users should use the web installer.

Install `esptool`:

```sh
pip3 install esptool
```

Download the `firmware-*.bin` file from the [releases page](https://github.com/uxjulia/CrossInk/releases), then connect your device with USB-C.

Find the device port:

```sh
# Linux
dmesg | grep tty

# macOS
ls /dev/cu.*
```

Flash the firmware:

```sh
# Linux
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin

# macOS
esptool.py --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

Replace the port and firmware path with your actual values.
