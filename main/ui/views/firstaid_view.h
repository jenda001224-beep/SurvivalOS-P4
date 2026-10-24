/*
 * @Description: SurvivalOS First Aid: what to do, the medicine cabinet,
 *               doses for each person and the herbs that help
 * @License: GPL 3.0
 */
#pragma once

#include "app/app_catalog.h"
#include "lvgl.h"
#include "ui/views/app_view_config.h"

namespace lilygo_box::ui {

lv_obj_t* CreateFirstAidView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config);

}  // namespace lilygo_box::ui
