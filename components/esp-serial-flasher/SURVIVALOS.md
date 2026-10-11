# esp-serial-flasher in SurvivalOS

Unmodified copy of Espressif's esp-serial-flasher **v2.0.0**
(<https://github.com/espressif/esp-serial-flasher>, commit `ca7edfc`, Apache-2.0),
without its examples, tests, docs and the Zephyr port, and without
`idf_component.yml` (which would pull USB host components this project does not
use).

SurvivalOS uses its SDIO protocol to write the ESP-Hosted firmware to the
ESP32-C6 from the ESP32-P4. The board-specific port (reset through the IO
expander, a slot shared with the SD card) lives in
`main/hal/device/common/coprocessor_sdio.cpp`; none of the library's own ports are
compiled (`CONFIG_SERIAL_FLASHER_PORT_*=n`).
