/*
 * @Description: Battery overview card (remaining time and charge level)
 * @License: GPL 3.0
 */
#pragma once

#include "hal/providers/battery_management_provider.h"
#include "lvgl.h"

namespace lilygo_box::ui {

// The card from Settings > Power Saving & Battery as a standalone widget, so
// the launcher can show it too. The settings page keeps its own copy of this
// code on purpose: that file is upstream's, and leaving it untouched keeps
// future merges simple. Looks and wording are the same in both.
struct BatteryOverviewCard {
  lv_obj_t* card = nullptr;
  lv_obj_t* fill = nullptr;
  lv_obj_t* time_label = nullptr;
  lv_obj_t* status_icon_label = nullptr;
  lv_obj_t* status_label = nullptr;
  int width = 0;
};

inline constexpr int kBatteryOverviewCardHeight = 190;

/**
 * @brief Creates the card with placeholder content.
 * @param parent Parent object.
 * @param width Card width in pixels.
 * @param card Receives the created objects.
 * @return true when every part of the card was created.
 */
bool CreateBatteryOverviewCard(
    lv_obj_t* parent, int width, BatteryOverviewCard* card);

/**
 * @brief Updates the fill level, the time estimate and the status line.
 * @param card Card created by CreateBatteryOverviewCard.
 * @param status Latest battery status.
 */
void UpdateBatteryOverviewCard(const BatteryOverviewCard& card,
    const hal::BatteryManagementStatus& status);

}  // namespace lilygo_box::ui
