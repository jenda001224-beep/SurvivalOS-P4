/*
 * @Description: SD 卡 MP3 曲库扫描与曲目信息实现
 * @Author: LILYGO_L
 * @Date: 2026-07-14 22:55:00
 * @LastEditTime: 2026-09-02 17:51:07
 * @License: GPL 3.0
 */
#include "app/music_library.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>

#include "audio/mp3_metadata.h"

namespace lilygo_box::app {
namespace {

constexpr int kMaximumDirectoryDepth = 16;
constexpr size_t kMaximumTrackCount = 2048;

/**
 * @brief 将 ASCII 字符串转换为小写形式
 * @param value 原始字符串
 * @return 小写字符串
 */
std::string AsciiLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
  return value;
}

/**
 * @brief 判断路径是否为 MP3 文件
 * @param path 文件路径
 * @return 扩展名为 mp3 返回 true，否则返回 false
 */
bool IsMp3Path(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  return dot != std::string::npos && AsciiLower(path.substr(dot)) == ".mp3";
}

bool IsPlaylistPath(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }
  const std::string extension = AsciiLower(path.substr(dot));
  return extension == ".m3u" || extension == ".m3u8";
}

bool FileExists(const std::string& path) {
  struct stat information{};
  return stat(path.c_str(), &information) == 0 && S_ISREG(information.st_mode);
}

std::string DirectoryOf(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string BaseName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A JPEG next to the song: its own, then the album's.
std::string FindSidecarCover(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
  for (const char* extension : {".jpg", ".jpeg", ".JPG"}) {
    if (FileExists(stem + extension)) {
      return stem + extension;
    }
  }
  const std::string directory = DirectoryOf(path);
  for (const char* name : {"cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg"}) {
    const std::string candidate = directory + "/" + name;
    if (FileExists(candidate)) {
      return candidate;
    }
  }
  return std::string();
}

/**
 * @brief 从文件路径中提取不带扩展名的文件名
 * @param path 文件路径
 * @return 文件标题
 */
std::string FileTitle(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t begin = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = path.find_last_of('.');
  const size_t end =
      dot == std::string::npos || dot < begin ? path.size() : dot;
  return path.substr(begin, end - begin);
}

/**
 * @brief 将目录与子项名称组合为路径
 * @param directory 目录路径
 * @param name 子项名称
 * @return 组合后的路径
 */
std::string JoinPath(const std::string& directory, const char* name) {
  if (!directory.empty() && directory.back() == '/') {
    return directory + name;
  }
  return directory + "/" + name;
}

/**
 * @brief 将一个 MP3 文件加入曲库
 * @param path MP3 文件路径
 * @param known_paths 已加入路径集合
 * @param tracks 曲库输出列表
 */
void AddMp3Track(const std::string& path,
    std::unordered_set<std::string>* known_paths,
    std::vector<MusicTrack>* tracks) {
  if (!known_paths->insert(path).second) {
    return;
  }
  audio::Mp3Metadata metadata;
  if (!audio::ReadMp3Metadata(path.c_str(), &metadata)) {
    return;
  }
  MusicTrack track;
  track.path = path;
  track.title = metadata.title.empty() ? FileTitle(path) : metadata.title;
  track.artist = metadata.artist.empty() ? "Unknown Artist" : metadata.artist;
  track.duration_ms = metadata.duration_ms;
  track.album = metadata.album;
  if (metadata.cover_size > 0 && metadata.cover_is_jpeg) {
    track.cover_offset = metadata.cover_offset;
    track.cover_size = metadata.cover_size;
  } else {
    track.cover_path = FindSidecarCover(path);
  }
  tracks->push_back(std::move(track));
}

struct PendingDirectory {
  std::string path;
  int depth = 0;
};

/**
 * @brief 使用堆内存中的待处理列表迭代扫描音乐源目录
 * @param root_directory 音乐源根目录
 * @param known_paths 已加入路径集合
 * @param tracks 曲库输出列表
 * @return 所有可访问目录读取成功返回 true，否则返回 false
 */
bool ScanDirectory(const std::string& root_directory,
    std::unordered_set<std::string>* known_paths,
    std::vector<MusicTrack>* tracks,
    std::vector<std::string>* playlist_paths) {
  std::vector<PendingDirectory> pending_directories;
  pending_directories.push_back({root_directory, 0});
  bool success = true;
  while (!pending_directories.empty() && tracks->size() < kMaximumTrackCount) {
    PendingDirectory directory = std::move(pending_directories.back());
    pending_directories.pop_back();

    DIR* handle = opendir(directory.path.c_str());
    if (handle == nullptr) {
      success = false;
      continue;
    }

    while (dirent* entry = readdir(handle)) {
      if (std::string(entry->d_name) == "." ||
          std::string(entry->d_name) == "..") {
        continue;
      }
      std::string path = JoinPath(directory.path, entry->d_name);
      struct stat information{};
      if (stat(path.c_str(), &information) != 0) {
        success = false;
        continue;
      }
      if (S_ISDIR(information.st_mode) &&
          directory.depth < kMaximumDirectoryDepth) {
        pending_directories.push_back({std::move(path), directory.depth + 1});
      } else if (S_ISREG(information.st_mode) && IsMp3Path(path)) {
        AddMp3Track(path, known_paths, tracks);
      } else if (S_ISREG(information.st_mode) && IsPlaylistPath(path) &&
          entry->d_name[0] != '.' && playlist_paths != nullptr) {
        playlist_paths->push_back(path);
      }
      if (tracks->size() >= kMaximumTrackCount) {
        break;
      }
    }
    closedir(handle);
  }
  return success;
}

// "a/./b/../c" -> "a/c", so playlist entries meet the scanned paths.
std::string NormalizePath(const std::string& path) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string part = path.substr(start, end - start);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    start = end + 1;
  }
  std::string out;
  for (const std::string& part : parts) {
    out += "/" + part;
  }
  return out;
}

void Trim(std::string* text) {
  while (!text->empty() && (text->back() == '\r' || text->back() == '\n' ||
                               text->back() == ' ' || text->back() == '\t')) {
    text->pop_back();
  }
  size_t begin = 0;
  while (begin < text->size() && ((*text)[begin] == ' ' || (*text)[begin] == '\t')) {
    ++begin;
  }
  text->erase(0, begin);
  // UTF-8 byte order mark at the start of the file.
  if (text->size() >= 3 && static_cast<unsigned char>((*text)[0]) == 0xEF &&
      static_cast<unsigned char>((*text)[1]) == 0xBB &&
      static_cast<unsigned char>((*text)[2]) == 0xBF) {
    text->erase(0, 3);
  }
}

bool ReadPlaylist(const std::string& path, const std::vector<MusicTrack>& tracks,
    MusicPlaylist* playlist) {
  FILE* file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    return false;
  }
  playlist->path = path;
  std::string name = BaseName(path);
  const size_t dot = name.find_last_of('.');
  playlist->name = dot == std::string::npos ? name : name.substr(0, dot);
  const std::string directory = DirectoryOf(path);
  char buffer[512];
  std::string pending_title;
  while (fgets(buffer, sizeof(buffer), file) != nullptr) {
    std::string line(buffer);
    Trim(&line);
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      if (line.rfind("#PLAYLIST:", 0) == 0 && line.size() > 10) {
        playlist->name = line.substr(10);
      } else if (line.rfind("#EXTINF:", 0) == 0) {
        const size_t comma = line.find(',');
        pending_title = comma == std::string::npos ? "" : line.substr(comma + 1);
      }
      continue;
    }
    std::replace(line.begin(), line.end(), '\\', '/');
    const std::string full = NormalizePath(line[0] == '/' ? line : directory + "/" + line);
    const std::string full_lower = AsciiLower(full);
    const std::string file_lower = AsciiLower(BaseName(line));
    int found = -1;
    for (size_t i = 0; i < tracks.size() && found < 0; ++i) {
      if (AsciiLower(NormalizePath(tracks[i].path)) == full_lower) {
        found = static_cast<int>(i);
      }
    }
    // Copied elsewhere: the same file name anywhere in the library.
    for (size_t i = 0; i < tracks.size() && found < 0; ++i) {
      if (AsciiLower(BaseName(tracks[i].path)) == file_lower) {
        found = static_cast<int>(i);
      }
    }
    // Renamed: "Artist - Title" from #EXTINF against the tags.
    for (size_t i = 0; i < tracks.size() && found < 0 && !pending_title.empty(); ++i) {
      const std::string tagged = AsciiLower(tracks[i].artist + " - " + tracks[i].title);
      if (tagged == AsciiLower(pending_title) ||
          AsciiLower(tracks[i].title) == AsciiLower(pending_title)) {
        found = static_cast<int>(i);
      }
    }
    if (found >= 0) {
      playlist->tracks.push_back(static_cast<size_t>(found));
    } else {
      ++playlist->missing;
    }
    pending_title.clear();
  }
  fclose(file);
  return true;
}

}  // namespace

bool ScanMusicLibrary(const std::vector<std::string>& source_paths,
    std::vector<MusicTrack>* tracks, std::vector<MusicPlaylist>* playlists) {
  if (tracks == nullptr) {
    return false;
  }
  tracks->clear();
  if (playlists != nullptr) {
    playlists->clear();
  }
  std::unordered_set<std::string> known_paths;
  std::vector<std::string> playlist_paths;
  bool success = true;
  for (const std::string& source_path : source_paths) {
    if (source_path.empty()) {
      continue;
    }
    success = ScanDirectory(source_path, &known_paths, tracks, &playlist_paths) && success;
  }
  std::sort(tracks->begin(), tracks->end(),
      [](const MusicTrack& left, const MusicTrack& right) {
        return AsciiLower(left.title) < AsciiLower(right.title);
      });
  if (playlists != nullptr) {
    std::sort(playlist_paths.begin(), playlist_paths.end());
    playlist_paths.erase(std::unique(playlist_paths.begin(), playlist_paths.end()),
        playlist_paths.end());
    for (const std::string& path : playlist_paths) {
      MusicPlaylist playlist;
      if (ReadPlaylist(path, *tracks, &playlist) &&
          (!playlist.tracks.empty() || playlist.missing > 0)) {
        playlists->push_back(std::move(playlist));
      }
    }
  }
  return success;
}

}  // namespace lilygo_box::app
