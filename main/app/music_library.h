/*
 * @Description: SD 卡 MP3 曲库扫描与曲目信息接口
 * @Author: LILYGO_L
 * @Date: 2026-07-14 22:55:00
 * @LastEditTime: 2026-07-14 22:55:00
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lilygo_box::app {

struct MusicTrack {
  std::string path;
  std::string title;
  std::string artist;
  uint32_t duration_ms = 0;
  // SurvivalOS: album and cover art. The cover is either embedded in the MP3
  // (JPEG at cover_offset, cover_size bytes) or a JPEG beside it
  // (cover_path: "<song>.jpg", "cover.jpg" or "folder.jpg").
  std::string album;
  uint32_t cover_offset = 0;
  uint32_t cover_size = 0;
  std::string cover_path;
};

// SurvivalOS: an M3U playlist from a music folder, resolved against the
// library. Entries whose file is missing are left out.
struct MusicPlaylist {
  std::string name;
  std::string path;
  std::vector<size_t> tracks;
  // Entries the playlist names but the card does not have.
  size_t missing = 0;
};

/**
 * @brief 递归扫描音乐源文件夹中的 MP3 文件
 * @param source_paths 音乐源文件夹绝对路径列表
 * @param tracks 扫描结果输出地址
 * @return 扫描过程正常完成返回 true，否则返回 false
 */
bool ScanMusicLibrary(const std::vector<std::string>& source_paths,
    std::vector<MusicTrack>* tracks,
    std::vector<MusicPlaylist>* playlists = nullptr);

}  // namespace lilygo_box::app
