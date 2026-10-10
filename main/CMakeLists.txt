set(src_dirs
    "."
    "app"
    "app/diagnostics"
    "app/storage"
    "audio"
    "base"
    "hal"
    "hal/ppa"
    "hal/providers"
    "survival"
    "ui"
    "ui/animation"
    "ui/input"
    "ui/resources/fonts/icons"
    "ui/resources/fonts/text"
    "ui/resources/images"
    "ui/theme"
    "ui/views"
    "ui/views/settings"
    "ui/widgets"
    "ui/widgets/prompt")

if(CONFIG_LILYGO_DEVICE_DRIVER_T_DISPLAY_P4)
    list(APPEND src_dirs
        "hal/device/common"
        "hal/device/t_display_p4"
        "hal/usb")
elseif(CONFIG_LILYGO_DEVICE_DRIVER_T_DISPLAY_P4_AIR)
    list(APPEND src_dirs
        "hal/device/common"
        "hal/device/t_display_p4_air"
        "hal/usb")
endif()

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS "."
    REQUIRES
        zstd
        esp_driver_ppa
        esp_lcd
        esp_timer
        esp_video
        lvgl
        lilygo_device_driver
    PRIV_REQUIRES
        app_update
        bootloader_support
        esp_app_format
        esp_common
        esp_audio_codec
        esp_driver_jpeg
        esp_driver_rmt
        esp_eth
        esp_event
        esp_hosted
        esp_http_client
        esp_https_ota
        fatfs
        esp_hw_support
        esp_netif
        nvs_flash
        esp_system
        esp_wifi
        freertos
        heap
        littlefs
        lwip
        mbedtls
        json
        spi_flash
        usb
        usb_host_msc)

# SurvivalOS: stamp each build so the device can show which one it is running.
# The timestamp is taken when CMake configures, and build.sh reconfigures before
# every build; only app/build_stamp.cpp sees the definition.
string(TIMESTAMP SURVIVALOS_BUILD_STAMP "%Y-%m-%d.%H%M")
set_source_files_properties(app/build_stamp.cpp PROPERTIES
    COMPILE_DEFINITIONS "SURVIVALOS_BUILD_STAMP=\"${SURVIVALOS_BUILD_STAMP}\"")
