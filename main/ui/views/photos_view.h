/*
 * @Description: Photos app view
 * @License: GPL 3.0
 */
#pragma once

#include "app/app_catalog.h"
#include "lvgl.h"
#include "ui/views/app_view_config.h"

namespace lilygo_box::ui {

/**
 * @brief Creates the photo library view.
 * @param parent Parent object.
 * @param app_entry Launcher entry the view was opened from.
 * @param config App view configuration.
 * @return The view root, or nullptr on failure.
 */
lv_obj_t* CreatePhotosView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config);

}  // namespace lilygo_box::ui
