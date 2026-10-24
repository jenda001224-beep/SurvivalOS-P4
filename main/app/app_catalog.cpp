/*
 * @Description: 主屏幕与底部停靠栏应用目录数据实现
 * @Author: LILYGO_L
 * @Date: 2026-05-10 13:27:05
 * @LastEditTime: 2026-05-10 23:28:15
 * @License: GPL 3.0
 */
#include "app/app_catalog.h"

namespace lilygo_box::app {
namespace {

constexpr AppEntry kHomeAppEntries[] = {
    {.id = "wikipedia", .title = "Wikipedia",
        .subtitle = "Offline encyclopaedia"},
    {.id = "maps", .title = "Maps", .subtitle = "Offline maps and GPS"},
    {.id = "photos", .title = "Photos", .subtitle = "Photo and video library"},
    {.id = "voicememo", .title = "Voice Memo",
        .subtitle = "Record and play audio notes"},
    {.id = "files", .title = "Files", .subtitle = "File manager"},
    {.id = "radio", .title = "Radio", .subtitle = "LoRa module control"},
    {.id = "nature", .title = "Nature", .subtitle = "Plants and fungi"},
    {.id = "petdoku", .title = "Petdoku", .subtitle = "Logic puzzle"},
    {.id = "cit", .title = "CIT", .subtitle = "Hardware self-test entry"},
};

constexpr AppEntry kDockAppEntries[] = {
    {.id = "camera", .title = "Camera", .subtitle = "Camera shortcut"},
    {.id = "music", .title = "Music", .subtitle = "Offline music player"},
    {.id = "settings", .title = "Settings", .subtitle = "System settings"},
};

constexpr size_t kHomeAppEntryCount =
    sizeof(kHomeAppEntries) / sizeof(kHomeAppEntries[0]);
constexpr size_t kDockAppEntryCount =
    sizeof(kDockAppEntries) / sizeof(kDockAppEntries[0]);
static_assert(kHomeAppEntryCount <= kMaxAppEntryCount);
static_assert(kDockAppEntryCount <= kMaxAppEntryCount);

const AppCatalog kHomeAppCatalog = {
    .entries = kHomeAppEntries,
    .entry_count = kHomeAppEntryCount,
};

const AppCatalog kDockAppCatalog = {
    .entries = kDockAppEntries,
    .entry_count = kDockAppEntryCount,
};

}  // namespace

const AppCatalog& GetHomeAppCatalog() { return kHomeAppCatalog; }

const AppCatalog& GetDockAppCatalog() { return kDockAppCatalog; }

const AppCatalog& GetAppCatalog() { return GetHomeAppCatalog(); }

}  // namespace lilygo_box::app
