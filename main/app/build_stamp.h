/*
 * @Description: Build identifier shown on the device and on the flasher page
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::app {

/**
 * @brief Returns when this firmware was built, e.g. "2026-10-10.1345".
 *
 * Every SurvivalOS build reports the same upstream version number, so this is
 * the only way to tell on the device which build is actually running. The same
 * text is copied into the flasher's manifest by tools/build_flash_image.py.
 */
const char* BuildStamp();

}  // namespace lilygo_box::app
