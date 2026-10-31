/*
 * @Description: Recently Deleted: photos wait in DCIM/.trash until the user
 *               removes them or the storage runs out of room
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

#include "hal/providers/storage_provider.h"

namespace lilygo_box::app {

// The folder inside the photo folder. Files in it are named
// "<deletion time>-<original name>", so the oldest deletion sorts first.
inline constexpr char kPhotoTrashDirectory[] = ".trash";

/**
 * @brief Moves a photo, and its cached thumbnail, to Recently Deleted.
 * @param photo_directory Full path of the photo folder.
 * @param name File name in that folder.
 */
bool MovePhotoToTrash(const char* photo_directory, const char* name);

/**
 * @brief Puts a photo back under its original name.
 * @param trash_name File name in the trash.
 */
bool RestorePhotoFromTrash(const char* photo_directory, const char* trash_name);

/**
 * @brief Deletes a photo from Recently Deleted for good.
 */
bool DeleteTrashedPhoto(const char* photo_directory, const char* trash_name);

/**
 * @brief How many photos wait in Recently Deleted.
 */
int CountTrashedPhotos(const char* photo_directory);

/**
 * @brief The name a trashed photo had before.
 * @return A pointer into trash_name.
 */
const char* OriginalPhotoName(const char* trash_name);

/**
 * @brief Frees room when the storage runs low, oldest deletions first, in a
 *        background task. Nothing is removed while there is room.
 */
void RequestPhotoTrashPurge(hal::StorageProvider* storage);

/**
 * @brief Test hook for the console: acts as if the storage were this low.
 */
void SetPhotoTrashLowBytesForTest(uint64_t bytes);

}  // namespace lilygo_box::app
