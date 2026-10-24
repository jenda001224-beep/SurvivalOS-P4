/*
 * @Description: SurvivalOS offline Translator: dictionary, word-by-word
 *               sentences, phrasebook and speech in every bundled language
 * @License: GPL 3.0
 */
#pragma once

#include "app/app_catalog.h"
#include "lvgl.h"
#include "ui/views/app_view_config.h"

namespace lilygo_box::ui {

/**
 * @brief Creates the Translator app.
 * @param parent LVGL parent.
 * @param app_entry Catalog entry.
 * @param config Size and device services.
 * @return Root object, or nullptr on failure.
 */
lv_obj_t* CreateTranslatorView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config);

}  // namespace lilygo_box::ui
