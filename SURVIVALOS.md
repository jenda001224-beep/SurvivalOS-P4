# SurvivalOS for the LILYGO T-Display-P4

An unofficial fork of LILYGO's [lilygobox-espidf](https://github.com/Xinyuan-LilyGO/lilygobox-espidf)
that turns the **T-Display-P4 V1.0** into an offline "survival phone". Not affiliated
with or endorsed by LILYGO.

**Status: early build.** It boots and runs on a real T-Display-P4 V1.0, but it is
work in progress and the newest changes may not have been tried on hardware yet.
Flashing is reversible: the web flasher can also write LILYGO's factory image back.

- **Web flasher:** <https://jenda001224-beep.github.io/SurvivalOS-P4/> (Chrome or Edge, desktop)
- **Based on:** `lilygobox-espidf` tag `v1.1.2`. `git diff v1.1.2` shows every change.
- **Modified:** from 2026-08-31 onwards; see the commit history for dates.

## What is added

- iOS-style shell: swipe up from the bottom edge to go home, swipe down from the top
  for a Control Center (brightness, volume, Wi-Fi, Bluetooth LE, silent mode, lock).
- Haptic feedback for system gestures and app launches.
- Offline Wikipedia reader (ZIM files, zstd, title search), split across files of at
  most 2 GiB because the SD card is FAT32 and file offsets are 32-bit.
- Offline vector maps (PMTiles v3 + Mapbox Vector Tiles, rendered on the device)
  with GPS position and a hiking-trail overlay.
- Photos, Voice Memo, photo and MJPEG/AVI video capture in Camera, WAV playback.
- Camera viewfinder that covers the whole screen with an enlarged centre crop of the
  sensor instead of a letterboxed 16:9 strip.
- Battery card (time remaining and charge) on the home page in place of the clock.
- SD cards formatted as exFAT, and GPT partition tables, are readable
  (`components/fatfs` overrides ESP-IDF's component, where exFAT is off).
- Upstream's compass app is backported from lilygobox-espidf `5db8622` (it is not
  part of v1.1.2) together with the IMU code it needs, but it is currently not listed
  on the home page.
- The Control Center shows whether the SD card and the Wi-Fi co-processor came up
  and, if not, what ESP-IDF logged about it, so a failure can be read off the screen.
- Dark theme by default; timed haptic effects, including a faint buzz while a slider
  is dragged.

Sources live mostly in `main/survival/`, `main/ui/views/`, `main/ui/widgets/control_center.*`
and `main/ui/input/system_gestures.*`.

## Hardware facts that matter when building

- The V1.0 board carries **ESP32-P4 silicon revision 1.x**. ESP-IDF >= 5.5.3 targets
  revision >= 3 by default and such an image does not boot here, so
  `sdkconfig.defaults.esp32p4` sets `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`.
- The side USB-C port (**P4.U**) goes through a CH343P to UART0, with RTS wired
  straight to EN and DTR straight to GPIO35. A serial port left open with RTS
  asserted holds the chip in reset.
- The ESP32-C6 co-processor is Bluetooth LE only, so there is no Bluetooth audio.
- The ESP32-C6 of early units runs **ESP-AT**, not the ESP-Hosted firmware this
  system (and LilygoBox) needs, so Wi-Fi does not come up on them. The phone
  replaces it by itself: `main/hal/device/t_display_p4/coprocessor.cpp` asks ESP-AT
  to force a download boot (`AT+SYSREG` on `LP_AON_SYS_CFG`, then `AT+RST`) and
  writes the vendor's ESP-Hosted image (`assets/coprocessor/`) over SDIO with
  esp-serial-flasher. Settings → WLAN offers it when needed. LILYGO's own route
  is a USB-UART adapter on the "C6 UART" connector.
- The camera driver's `VIDIOC_DQBUF` blocks regardless of `O_NONBLOCK`, and it
  cannot complete a buffer while the other of its two is held: never dequeue a
  second frame before giving the first one back.

## Diagnostics over USB

With the device on its P4.U port, lines starting with `svos` on the serial
console (115200 baud) are commands; `svos help` lists them. They report what the
device sees (`svos status`), the CPU share of each task (`svos tasks`), and start
the few actions whose logs matter: Wi-Fi on/off/scan, an SD card write-and-read
test, the camera preview, and identifying or updating the Wi-Fi chip
(`svos c6 identify | rehearse | install`). See
`main/app/diagnostics/serial_commands.cpp`.

## Build

Requires ESP-IDF v5.5.4 or later (built with v5.5.5).

```bash
git clone --recursive https://github.com/jenda001224-beep/SurvivalOS-P4.git
cd SurvivalOS-P4
idf.py set-target esp32p4
idf.py build
python3 tools/build_flash_image.py   # merges and validates docs/survivalos-p4.bin
node tools/test_flasher.mjs          # drives docs/index.html against a simulated ESP32-P4
```

`tools/build_flash_image.py` refuses to produce an image that would not boot on this
board (wrong chip, chip revision >= 3, wrong flash size, different partition table,
non-blank otadata, bad checksum).

## Licence

GPL-3.0, like the firmware it is derived from; see [LICENSE](./LICENSE). The upstream
README follows unchanged in [README.md](./README.md). Third-party parts keep their own
licences: zstd (`components/zstd`, BSD), FatFs and its ESP-IDF glue
(`components/fatfs`, BSD-style and Apache-2.0), esptool-js (`docs/vendor`, Apache-2.0),
esp-serial-flasher (`components/esp-serial-flasher`, Apache-2.0) and the ESP-Hosted
firmware for the ESP32-C6 as built by LILYGO (`assets/coprocessor`, Apache-2.0).
