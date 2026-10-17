/*
 * @Description: UI haptic feedback helpers
 * @Author: LILYGO_L
 * @Date: 2026-06-30 00:00:00
 * @LastEditTime: 2026-06-30 00:00:00
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::hal {
class HapticProvider;
}  // namespace lilygo_box::hal

namespace lilygo_box::ui {

// Distinct feels for the different classes of UI interaction. Each one maps to
// an AW86224 RAM waveform plus a gain scale in haptic_feedback.cpp; the mapping
// is the only place that needs tuning once the effects are felt on hardware.
enum class HapticEffect {
  // Light tick for list selection, slider detents and toggles.
  kSelection,
  // Regular tap played when a control is activated.
  kImpactLight,
  // Firmer tap for opening an app.
  kImpactMedium,
  // Distinct thump for the home gesture and other system-level transitions.
  kImpactHeavy,
  // Played when a gesture crosses the threshold that commits it.
  kGestureCommit,
  // Played when an action completes successfully.
  kSuccess,
  // Played when an action is refused or fails.
  kWarning,
  // Faint, slightly longer vibration repeated while a slider is dragged, so
  // the motion itself can be felt rather than only its end points.
  kSliderMove,
  // Barely-there tick for marking a cell in a puzzle.
  kPuzzleTap,
  // Even fainter tick for each cell crossed while marking by dragging.
  kPuzzleDrag,
};

/**
 * @brief 注册 UI 交互振动反馈提供者
 * @param haptic 振动反馈提供者指针
 */
void RegisterUiHapticProvider(hal::HapticProvider* haptic);

/**
 * @brief 设置 UI 交互振动反馈偏好
 * @param enabled true 表示启用振动反馈，false 表示关闭振动反馈
 * @param strength_percent 振动强度百分比，范围 0~100
 */
void SetUiHapticPreferences(bool enabled, int strength_percent);

/**
 * @brief 播放一次 UI 交互振动反馈
 */
void PlayUiHapticFeedback();

/**
 * @brief Plays the haptic effect that matches a given class of interaction.
 * @param effect Interaction class to play.
 */
void PlayUiHapticEffect(HapticEffect effect);

}  // namespace lilygo_box::ui
