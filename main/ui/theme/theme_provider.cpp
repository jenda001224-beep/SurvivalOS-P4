/*
 * @Description: UI theme provider
 * @Author: LILYGO_L
 * @Date: 2026-06-25 00:00:00
 * @LastEditTime: 2026-08-25 00:00:00
 * @License: GPL 3.0
 */
#include "ui/theme/theme_provider.h"

#include "app/power_saving.h"

namespace lilygo_box::ui::theme {
namespace {

const ThemeColors* g_active_theme_colors = &kLightTheme;

/**
 * @brief 获取指定主题模式对应的颜色配置
 * @param mode 主题模式
 * @return 主题颜色配置
 */
constexpr const ThemeColors& ResolveThemeColors(ThemeMode mode) {
  switch (mode) {
    case ThemeMode::kLight:
      return kLightTheme;
    case ThemeMode::kDark:
      return kDarkTheme;
  }
  return kLightTheme;
}

}  // namespace

ThemeMode ThemeProvider::mode() const { return mode_; }

bool ThemeProvider::is_dark() const { return mode_ == ThemeMode::kDark; }

const ThemeColors& ThemeProvider::colors() const { return *colors_; }

void ThemeProvider::SetMode(ThemeMode mode) {
  mode_ = mode;
  colors_ = &ResolveThemeColors(mode);
  g_active_theme_colors = colors_;
}

const ThemeColors& GetThemeColors(const ThemeProvider* provider) {
  return provider == nullptr ? ActiveThemeColors() : provider->colors();
}

namespace {

// SurvivalOS: Low Power Mode draws on pure black (a black AMOLED pixel is off),
// whichever theme is chosen, so the text has to be the dark theme's.
ThemeColors MakeBlackTheme() {
  ThemeColors colors = kDarkTheme;
  colors.surface = 0x000000;
  colors.surface_dim = 0x000000;
  colors.surface_container_lowest = 0x000000;
  colors.wallpaper_background = 0x000000;
  colors.wallpaper_layer_1 = 0x000000;
  colors.wallpaper_layer_2 = 0x000000;
  colors.wallpaper_layer_3 = 0x000000;
  colors.wallpaper_layer_4 = 0x000000;
  return colors;
}

}  // namespace

const ThemeColors& ActiveThemeColors() {
  static const ThemeColors black_theme = MakeBlackTheme();
  return app::IsLowPowerModeEnabled() ? black_theme : *g_active_theme_colors;
}

}  // namespace lilygo_box::ui::theme
