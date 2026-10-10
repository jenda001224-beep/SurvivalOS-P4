/*
 * @Description: UI haptic feedback helpers
 * @Author: LILYGO_L
 * @Date: 2026-06-30 00:00:00
 * @LastEditTime: 2026-09-02 17:54:10
 * @License: GPL 3.0
 */
#include "ui/haptic_feedback.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "app/storage/haptic_storage.h"
#include "hal/providers/haptic_provider.h"

namespace lilygo_box::ui {
namespace {

// How each interaction class is rendered on the AW86224. `sequence` picks a
// waveform out of the RAM library the driver loads (kRam12k041230_235) and
// `gain_percent` scales it against the user's strength preference. The numbers
// are starting points chosen so the effects differ in weight; they are the one
// thing worth re-tuning by feel once running on the board.
struct HapticEffectMapping {
  uint8_t sequence;
  uint8_t loop_count;
  int gain_percent;
  bool auto_brake;
  // How long the motor is left running. Around 10 ms is a click; from roughly
  // 25 ms up it is felt as a vibration.
  uint32_t play_ms;
};

// kRam12k041230_235 holds exactly four waveforms (its address table lists four
// start/end pairs: three ~18 ms pulses and one 4 ms tick). The chip driver
// rejects any other sequence number and the effect is then silently dropped,
// so every mapping below has to stay within 1..4.
constexpr uint8_t kRamLibraryWaveformCount = 4;
// Loop count that keeps repeating the waveform until play_ms is over.
constexpr uint8_t kLoopUntilStopped = 16;

constexpr HapticEffectMapping kEffectMappings[] = {
    /* kSelection     */ {.sequence = 1, .loop_count = 1, .gain_percent = 55,
        .auto_brake = true, .play_ms = 10},
    /* kImpactLight   */ {.sequence = 1, .loop_count = 1, .gain_percent = 80,
        .auto_brake = true, .play_ms = 10},
    /* kImpactMedium  */ {.sequence = 2, .loop_count = 1, .gain_percent = 100,
        .auto_brake = true, .play_ms = 12},
    /* kImpactHeavy   */ {.sequence = 3, .loop_count = 1, .gain_percent = 100,
        .auto_brake = true, .play_ms = 16},
    /* kGestureCommit */ {.sequence = 1, .loop_count = 1, .gain_percent = 70,
        .auto_brake = true, .play_ms = 10},
    /* kSuccess       */ {.sequence = 2, .loop_count = kLoopUntilStopped,
        .gain_percent = 80, .auto_brake = true, .play_ms = 45},
    /* kWarning       */ {.sequence = 3, .loop_count = kLoopUntilStopped,
        .gain_percent = 100, .auto_brake = true, .play_ms = 90},
    /* kSliderMove    */ {.sequence = 1, .loop_count = kLoopUntilStopped,
        .gain_percent = 28, .auto_brake = true, .play_ms = 28},
};

hal::HapticProvider* g_haptic_provider = nullptr;
bool g_haptic_preferences_loaded = false;
bool g_haptic_enabled = true;
int g_haptic_strength_percent = 90;

/**
 * @brief 确保已从长期 RAM 缓存加载 UI 交互振动反馈偏好
 */
void EnsureHapticPreferencesLoaded() {
  if (g_haptic_preferences_loaded) {
    return;
  }

  app::HapticPreferences preferences = app::GetHapticPreferences();
  g_haptic_enabled = preferences.enabled;
  g_haptic_strength_percent = preferences.strength_percent;
  g_haptic_preferences_loaded = true;
}

}  // namespace

void RegisterUiHapticProvider(hal::HapticProvider* haptic) {
  g_haptic_provider = haptic;
}

void SetUiHapticPreferences(bool enabled, int strength_percent) {
  g_haptic_enabled = enabled;
  g_haptic_strength_percent = std::clamp(strength_percent, 0, 100);
  g_haptic_preferences_loaded = true;
}

void PlayUiHapticFeedback() { PlayUiHapticEffect(HapticEffect::kImpactLight); }

void PlayUiHapticEffect(HapticEffect effect) {
  if (g_haptic_provider == nullptr) {
    return;
  }

  EnsureHapticPreferencesLoaded();
  if (!g_haptic_enabled) {
    return;
  }

  const size_t index = static_cast<size_t>(effect);
  if (index >= sizeof(kEffectMappings) / sizeof(kEffectMappings[0])) {
    return;
  }
  const HapticEffectMapping& mapping = kEffectMappings[index];
  const int scaled_percent =
      g_haptic_strength_percent * mapping.gain_percent / 100;
  const uint8_t gain = static_cast<uint8_t>(
      std::clamp(scaled_percent, 0, 100) * UINT8_MAX / 100);
  // Fall back to the stock UI waveform rather than lose the effect entirely
  // if a mapping ever points outside the loaded library.
  const uint8_t sequence =
      mapping.sequence >= 1 && mapping.sequence <= kRamLibraryWaveformCount
          ? mapping.sequence
          : 1;
  g_haptic_provider->PlayHapticWaveformFor(
      sequence, mapping.loop_count, gain, mapping.auto_brake, mapping.play_ms);
}

}  // namespace lilygo_box::ui
