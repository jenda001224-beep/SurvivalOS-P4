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
  for a Control Center (brightness, volume, Wi-Fi, Low Power Mode, silent mode, lock).
- A setup assistant on the first start (`main/ui/views/setup_wizard_view.*`):
  edition **Full** (with an SD card) or **Mini** (no card: the card-only apps are
  hidden and photos and memos go to internal memory), SD card check, a pick of maps
  and Wikipedia, and a QR code to the web page's download section with that pick
  ticked (`#content?maps=cz&wiki=cs,en`), then Wi-Fi. It has its own NVS flag
  (`svos/setup`), so a phone updated with "keep settings" still sees it once;
  Settings > More Settings > Run setup again shows it again.
- Low Power Mode (`main/app/power_saving.*`): brightness capped at 30 %, the Wi-Fi
  co-processor powered off (and back on afterwards if it was in use), 15 frames per
  second with touch read every 20 ms, auto-lock after 30 s. It turns on by itself at
  20 % on battery and off again once charging reaches 80 %. The battery icon turns
  yellow while it is on.
- Survival Mode (Settings > Power Saving & Battery, `svos survival on`): Low
  Power Mode's limits (screen max 25 %, auto-lock 20 s) plus only Nature, Maps
  and Radio on the home screen, and four seconds after the screen goes dark the
  whole phone enters deep sleep. The touch panel stays in gesture mode; a double
  tap pulls its interrupt, the XL9535 passes it to GPIO5 (an LP pin, ext1 wake)
  and the phone restarts straight into the lock screen
  (`TDisplayP4Device::PrepareSurvivalDeepSleep`, `Application::EnterSurvivalDeepSleep`).
- Nature, a field guide to every plant and fungus recorded in Czechia (about
  5,600 species): Czech, Latin and English names, up to three photos each, what
  it is good for (edible, medicinal, poisonous, deadly...), habitats, the months
  it is seen and the Czech Wikipedia text. Search ignores diacritics; filters by
  group, use, colour, habitat and season. Data: one file, `nature.svnat`, built by
  `tools/prepare_nature.py` from iNaturalist and Wikipedia (photos keep their
  CC licences and credits, shown under each photo). `main/ui/views/nature_view.*`,
  `main/survival/nature_pack.*`; `hosttest/nature/`.
- The UI text fonts (22-36 px) now carry Latin-1 and Latin Extended-A, so Czech
  (and most European) text shows its accents, in Wikipedia too.
- Downloads copied anywhere on the card (top folder or one level below) are moved
  to `/wiki`, `/maps`, `/nature` and `/Music` when the card is mounted
  (`main/app/content_sorter.*`; `hosttest/sorter/`).
- Haptic feedback for system gestures and app launches.
- Offline Wikipedia reader (ZIM files, zstd, title search). Files on the SD card are
  read through FatFs directly with 64-bit offsets, because newlib's `fseek`/`ftell`
  stop at 2 GiB; a single 50 GB Kiwix file works on an exFAT card, and split parts
  (`.zimaa`, `.zimab`, ...) still do too (`main/survival/split_file.*`;
  `hosttest/splitfile/` reads a 4.5 GiB file on a real exFAT image).
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

## Petdoku

A logic puzzle in place of the planned ClassiCube: an N x N grid (6 to 10) split
into N regions, one pet in every row, column and region, none touching. Puzzles
are generated on the device (`main/games/petdoku_engine.*`): a random solution,
regions grown around it, reshaped until no second solution exists, and kept only
when a solver that never guesses can finish it. `hosttest/petdoku/` in the project
folder checks uniqueness independently by search. The puzzle number is the seed,
so "No. 12" is the same puzzle everywhere.

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
(`svos c6 identify | rehearse | install`). Also `svos jpeg` (colour round trip
through the hardware JPEG encoder and the photo viewer's decoder), `svos lowpower
on|off`, `svos setup` (assistant again at the next start), `svos edition full|mini`
and `svos sort` (file the card's downloads now). See
`main/app/diagnostics/serial_commands.cpp`.

Photos once came out in wrong colours: the photo viewer asked the JPEG decoder for
`JPEG_DEC_RGB_ELEMENT_ORDER_RGB`, which on the P4 swaps the two bytes of every
RGB565 pixel (big endian) while LVGL draws little endian. It now asks for `BGR`,
the order the ESP-IDF LCD example uses; the encoder side is unchanged.

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
