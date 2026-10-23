/*
 * @Description: SurvivalOS setup assistant shown on the first start
 * @License: GPL 3.0
 */
#pragma once

#include <functional>

#include "app/edition.h"
#include "lvgl.h"

namespace lilygo_box::hal {
class StorageProvider;
}

namespace lilygo_box::ui {

// What the user picked, handed over when the assistant finishes.
struct SetupWizardResult {
  app::Edition edition = app::Edition::kFull;
  app::MapsChoice maps = app::MapsChoice::kNone;
  app::WikipediaChoice wikipedia = app::WikipediaChoice::kNone;
  // Open Settings > WLAN once the assistant has closed.
  bool open_wifi_settings = false;
};

struct SetupWizardViewOptions {
  int screen_width = 0;
  int screen_height = 0;
  // Used to look for the SD card while the assistant is open; may be null.
  hal::StorageProvider* storage = nullptr;
  // Called once with the choices; returning false keeps the assistant open.
  std::function<bool(const SetupWizardResult&)> completion_callback;
};

/**
 * @brief Creates the full-screen setup assistant.
 * @param parent LVGL parent.
 * @param options Size, storage and completion callback.
 * @return The assistant's root object, or nullptr on failure.
 */
lv_obj_t* CreateSetupWizardView(
    lv_obj_t* parent, const SetupWizardViewOptions& options);

/**
 * @brief Builds the content page link for a selection.
 *
 * The same text goes into the QR code, so the page opens with the right
 * files already picked.
 *
 * @param maps Map choice.
 * @param wikipedia Wikipedia choice.
 * @param buffer Output.
 * @param size Output size.
 */
void FormatSetupContentUrl(app::MapsChoice maps,
    app::WikipediaChoice wikipedia, char* buffer, size_t size);

}  // namespace lilygo_box::ui
