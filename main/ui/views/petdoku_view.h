/*
 * @Description: Petdoku, a logic puzzle on a grid of regions
 * @License: GPL 3.0
 */
#pragma once

#include "app/app_catalog.h"
#include "lvgl.h"
#include "ui/views/app_view_config.h"

namespace lilygo_box::ui {

/**
 * @brief Creates the Petdoku game view.
 * @param parent Parent object.
 * @param app_entry Catalog entry the view was opened from.
 * @param config Size and device services of the view.
 * @return The view, or nullptr when it could not be created.
 */
lv_obj_t* CreatePetdokuView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config);

}  // namespace lilygo_box::ui
