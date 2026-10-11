# ESP32-C6 firmware bundled with SurvivalOS

`esp32c6-network-adapter-v2.12.3.svc6` is the ESP-Hosted network-adapter firmware
for the ESP32-C6 of the T-Display-P4 V1.0, as published by LILYGO with LilygoBox:

| | |
| --- | --- |
| Source | <https://github.com/Xinyuan-LilyGO/lilygobox-espidf/releases/tag/v1.1.2> |
| File | `lilygobox-t-display-p4-device-v1.0-esp32c6-rev0.0-v2.12.3-merged.bin` |
| Size | 1 272 064 bytes, flashed at `0x0` |
| SHA-256 | `57fb270d5e5554fd8adf7a408e59fb2215b1ca5adea8485bb7c5f4b5d877b82b` |
| MD5 | `d3f97128ac5bc21ebe8979ad3213219f` |
| Contents | bootloader, partition table, `network_adapter` 2.12.3 (ESP-IDF v5.5.4) |

It is ESP-Hosted-MCU (Apache-2.0, <https://github.com/espressif/esp-hosted-mcu>),
built by LILYGO for this board. The `.svc6` file is the same image deflated, with
a 40-byte header; `tools/pack_coprocessor_firmware.py` documents and produces it:

```sh
tools/pack_coprocessor_firmware.py <merged.bin> assets/coprocessor/<name>.svc6
```

Why the phone carries it: early units have ESP-AT on the ESP32-C6, which this
firmware cannot use for Wi-Fi. `main/hal/device/t_display_p4/coprocessor.cpp`
writes this image to the chip over SDIO, so no USB-UART adapter is needed.
