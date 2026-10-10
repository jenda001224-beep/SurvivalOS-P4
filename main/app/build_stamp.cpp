/*
 * @Description: Build identifier shown on the device and on the flasher page
 * @License: GPL 3.0
 */
#include "app/build_stamp.h"

#ifndef SURVIVALOS_BUILD_STAMP
#define SURVIVALOS_BUILD_STAMP "unknown"
#endif

namespace lilygo_box::app {
namespace {

// The "SVBUILD=" prefix is how tools/build_flash_image.py finds the stamp in
// the finished binary. main/CMakeLists.txt defines the stamp for this file
// only, at configure time, so one small file is recompiled per build.
constexpr char kMarkedStamp[] = "SVBUILD=" SURVIVALOS_BUILD_STAMP;
constexpr int kMarkerLength = 8;

}  // namespace

const char* BuildStamp() { return kMarkedStamp + kMarkerLength; }

}  // namespace lilygo_box::app
