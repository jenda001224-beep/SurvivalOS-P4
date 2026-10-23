/*
 * @Description: iOS-style control center overlay
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <functional>

#include "lvgl.h"

namespace lilygo_box::ui {

// Everything the control center needs in order to read and write live system
// state. The shell owns the actual providers; the panel only calls back.
struct ControlCenterConfig {
  // Current display brightness as a percentage.
  std::function<int()> get_brightness_percent;
  // Applies a new display brightness; returns false when it was rejected.
  std::function<bool(int percent)> set_brightness_percent;
  // Current output volume as a percentage.
  std::function<int()> get_volume_percent;
  // Applies a new output volume; returns false when it was rejected.
  std::function<bool(int percent)> set_volume_percent;
  // Reads and writes the Wi-Fi radio switch.
  std::function<bool()> get_wifi_enabled;
  std::function<void(bool enabled)> set_wifi_enabled;
  // Highest brightness the slider may reach right now (Low Power Mode caps
  // it); unset means 100.
  std::function<int()> get_brightness_limit_percent;
  // Reads and writes Low Power Mode. SurvivalOS: this slot held a Bluetooth
  // switch upstream, but nothing on the phone uses Bluetooth.
  std::function<bool()> get_low_power_enabled;
  std::function<void(bool enabled)> set_low_power_enabled;
  // Reads and writes silent mode.
  std::function<bool()> get_silent_mode;
  std::function<void(bool enabled)> set_silent_mode;
  // Locks the screen straight from the panel.
  std::function<void()> request_screen_lock;
  // Writes a short plain-text summary of the SD card and the Wi-Fi chip,
  // including why either failed, for the status area under the sliders.
  std::function<void(char* buffer, size_t buffer_size)> describe_hardware;
};

// Pull-down panel that lives on the LVGL top layer, above every app view.
class ControlCenter final {
 public:
  ControlCenter() = default;
  ~ControlCenter() = default;

  ControlCenter(const ControlCenter&) = delete;
  ControlCenter& operator=(const ControlCenter&) = delete;

  /**
   * @brief Builds the panel, hidden, on the LVGL top layer.
   * @param config Callbacks into the live system state.
   * @return true when the panel was created.
   */
  bool Init(const ControlCenterConfig& config);

  /**
   * @brief Deletes the panel and forgets its objects.
   */
  void Destroy();

  /**
   * @brief Rebuilds the panel for a new screen size or theme.
   * @return true when the panel was rebuilt.
   */
  bool Relayout();

  /**
   * @brief Tracks the panel to a live pull-down gesture.
   * @param progress_permille Gesture progress, 0..1000.
   */
  void SetDragProgress(int progress_permille);

  /**
   * @brief Animates the panel fully open and refreshes its controls.
   */
  void Open();

  /**
   * @brief Animates the panel closed.
   */
  void Close();

  /**
   * @brief Reports whether the panel is open or opening.
   * @return true when open.
   */
  bool IsOpen() const { return open_; }

  /**
   * @brief Reports whether any part of the panel is on screen.
   * @return true when the panel is visible.
   */
  bool IsVisible() const;

  /**
   * @brief Re-reads every live value into the controls.
   */
  void Refresh();

 private:
  // One of the round on/off buttons in the top row.
  struct ToggleButton {
    lv_obj_t* button = nullptr;
    lv_obj_t* icon = nullptr;
    bool active = false;
  };

  /**
   * @brief Handles a tap on one of the round toggles.
   * @param event LVGL event.
   */
  static void ToggleClickedCallback(lv_event_t* event);

  /**
   * @brief Handles a change on the brightness or volume slider.
   * @param event LVGL event.
   */
  static void SliderChangedCallback(lv_event_t* event);

  /**
   * @brief Handles a tap on the dimmed backdrop behind the panel.
   * @param event LVGL event.
   */
  static void BackdropClickedCallback(lv_event_t* event);

  /**
   * @brief Moves the panel and fades the backdrop for one animation step.
   * @param target Control center instance.
   * @param value Panel offset from its fully open position, in pixels.
   */
  static void SetPanelOffset(void* target, int32_t value);

  /**
   * @brief Hides the panel once the closing animation has finished.
   * @param animation LVGL animation.
   */
  static void CloseCompletedCallback(lv_anim_t* animation);

  /**
   * @brief Creates one round toggle button.
   * @param parent Parent object.
   * @param toggle Toggle to fill in.
   * @param symbol Icon glyph.
   * @param x Horizontal position inside the parent.
   * @param y Vertical position inside the parent.
   * @param size Button diameter.
   * @return true when the toggle was created.
   */
  bool CreateToggle(lv_obj_t* parent, ToggleButton* toggle, const char* symbol,
      int x, int y, int size);

  /**
   * @brief Creates one tall iOS-style slider.
   * @param parent Parent object.
   * @param symbol Icon glyph drawn at the bottom of the track.
   * @param x Horizontal position inside the parent.
   * @param y Vertical position inside the parent.
   * @param width Slider width.
   * @param height Slider height.
   * @return The slider, or nullptr on failure.
   */
  lv_obj_t* CreateSlider(lv_obj_t* parent, const char* symbol, int x, int y,
      int width, int height);

  /**
   * @brief Paints a toggle in its on or off colours.
   * @param toggle Toggle to paint.
   * @param active New state.
   */
  void ApplyToggleState(ToggleButton* toggle, bool active);

  /**
   * @brief Distance the panel travels between closed and open.
   * @return Travel distance in pixels.
   */
  int PanelTravel() const;

  ControlCenterConfig config_;
  lv_obj_t* backdrop_ = nullptr;
  lv_obj_t* panel_ = nullptr;
  lv_obj_t* brightness_slider_ = nullptr;
  lv_obj_t* volume_slider_ = nullptr;
  lv_obj_t* hardware_label_ = nullptr;
  ToggleButton wifi_toggle_;
  ToggleButton low_power_toggle_;
  ToggleButton silent_toggle_;
  ToggleButton lock_toggle_;
  int panel_height_ = 0;
  int panel_top_margin_ = 0;
  bool open_ = false;
};

}  // namespace lilygo_box::ui
