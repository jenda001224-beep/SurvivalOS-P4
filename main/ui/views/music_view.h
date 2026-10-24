/*
 * @Description: Music app view
 * @Author: LILYGO_L
 * @Date: 2026-07-08 00:00:00
 * @LastEditTime: 2026-07-08 00:00:00
 * @License: GPL 3.0
 */
#pragma once

#include "app/app_catalog.h"
#include "lvgl.h"
#include "ui/views/app_view_config.h"

namespace lilygo_box::ui {

/**
 * @brief 创建音乐应用界面
 * @param parent 父对象
 * @param app_entry 应用条目
 * @param config 应用视图配置
 * @return 创建成功返回音乐应用根对象，否则返回 nullptr
 */
lv_obj_t* CreateMusicView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config);

// SurvivalOS: what the music player is doing, for the lock screen's player.
// Call on the LVGL task; the strings stay valid until the library changes.
struct MusicNowPlaying {
  bool active = false;  // a track is loaded (playing or paused)
  bool playing = false;
  const char* title = "";
  const char* artist = "";
  uint32_t elapsed_ms = 0;
  uint32_t duration_ms = 0;
  // Changes whenever another track starts, so a cover is decoded once.
  uint32_t track_serial = 0;
};

/**
 * @brief Reads the player's state.
 * @param out Receives it.
 * @return true when a track is loaded.
 */
bool GetMusicNowPlaying(MusicNowPlaying* out);

/**
 * @brief Pauses or resumes the current track.
 */
void MusicTogglePlayback();

/**
 * @brief Next (1) or previous (-1) track, following the playlist if one plays.
 */
void MusicSkipTrack(int direction);

/**
 * @brief Jumps within the current track.
 * @param position_ms Position from the start.
 */
void MusicSeekTo(uint32_t position_ms);

/**
 * @brief Puts the current track's cover into an object, filling it.
 * @param artwork Object of the cover's size; its children are replaced.
 * @return true when the track has a cover.
 */
bool SetMusicNowPlayingCover(lv_obj_t* artwork);

}  // namespace lilygo_box::ui
