/*
 * @Description: Files the downloads copied onto the SD card into the folders
 *               the apps read
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>

namespace lilygo_box::app {

/**
 * @brief Moves content files on the card into place.
 *
 * The web page tells people to copy what they downloaded anywhere on the
 * card. This looks at the card's top folder and one level below it and moves
 * `.zim` (and split `.zimaa`...) files to /wiki, `.pmtiles` / `.svtrails` to
 * /maps, the field guide (`.svnat`) to /nature and loose `.mp3` files in the
 * top folder to /Music. A move on the
 * same card only renames, so even a 50 GB file takes no time. Files already
 * in place, macOS "._" shadow files and anything with a name clash are left
 * alone.
 *
 * @param base_path Mount point of the card, e.g. "/sdcard".
 * @return Number of files moved.
 */
size_t SortCardContent(const char* base_path);

/**
 * @brief Runs SortCardContent() on its own task, so mounting never waits.
 * @param base_path Mount point of the card; must stay valid (a constant).
 */
void StartCardContentSort(const char* base_path);

/**
 * @brief Whether a sort started by StartCardContentSort() is still moving files.
 * @return true while it runs.
 */
bool IsCardContentSortRunning();

}  // namespace lilygo_box::app
