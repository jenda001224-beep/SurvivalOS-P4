/**
 * @Description: Bluetooth preference storage
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::app {

// Bluetooth user preference. The wireless co-processor on this board is
// BLE-only, so this switch never controls audio devices; it gates BLE
// scanning and connections.
struct BluetoothPreferences {
  // Whether the user wants the BLE radio enabled.
  bool enabled_requested = false;
};

/**
 * @brief Loads the Bluetooth preference cache from NVS.
 */
void InitBluetoothCache();

/**
 * @brief Reads the current Bluetooth preference.
 * @return The cached preference.
 */
BluetoothPreferences GetBluetoothPreferences();

/**
 * @brief Stores a new Bluetooth preference and persists it.
 * @param preferences New preference snapshot.
 * @return true when the value was unchanged or successfully persisted.
 */
bool UpdateBluetoothPreferences(const BluetoothPreferences& preferences);

}  // namespace lilygo_box::app
