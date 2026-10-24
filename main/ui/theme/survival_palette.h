/*
 * @Description: Survival Mode look: black everywhere, dim red text
 * @License: GPL 3.0
 *
 * On the AMOLED panel a black pixel is switched off and costs nothing, and a
 * red one lights a single subpixel. Survival Mode therefore drops wallpapers,
 * fills and coloured tiles and draws only thin red text and outlines on black.
 */
#pragma once

#include <cstdint>

#include "app/power_saving.h"

namespace lilygo_box::ui::survival_look {

inline constexpr uint32_t kText = 0xC8301A;     // main text, icons
inline constexpr uint32_t kDim = 0x7A1E10;      // secondary text
inline constexpr uint32_t kFaint = 0x2E0B06;    // hairlines, pressed states
inline constexpr uint32_t kBlack = 0x000000;

inline bool Active() { return app::IsSurvivalModeEnabled(); }

}  // namespace lilygo_box::ui::survival_look
