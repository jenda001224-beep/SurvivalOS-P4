/*
 * @Description: SurvivalOS edition (FULL with an SD card, MINI without) and
 *               the first-run setup state
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>

namespace lilygo_box::hal {
class StorageProvider;
}

namespace lilygo_box::app {

enum class Edition : unsigned char {
  // Maps, Wikipedia, music and the file manager on an SD card.
  kFull = 1,
  // No card: everything that works from the phone's own memory.
  kMini = 2,
};

// What the setup assistant offered for the card. The web page that serves the
// files reads the same names from the QR code.
enum class MapsChoice : unsigned char { kNone, kCzechia, kWorld };
enum class WikipediaChoice : unsigned char { kNone, kCzech, kEnglish, kBoth };

/**
 * @brief Loads the saved edition and setup state. Call once after NVS is up.
 */
void InitEditionStorage();

/**
 * @brief The chosen edition; FULL until setup says otherwise.
 * @return Edition.
 */
Edition GetEdition();

/**
 * @brief Saves the edition.
 * @param edition New edition.
 */
void SetEdition(Edition edition);

/**
 * @brief Whether an app belongs on the home screen in the current edition.
 * @param app_id Catalog id, e.g. "maps".
 * @return true when the app should be shown.
 */
bool IsAppInEdition(const char* app_id);

/**
 * @brief Whether the setup assistant has run on this phone.
 *
 * Kept apart from the vendor's first-boot flag on purpose: a phone that was
 * set up before SurvivalOS had an assistant still gets to see it once, even
 * when it was flashed with "keep settings".
 *
 * @return true once setup finished.
 */
bool IsSetupCompleted();

/**
 * @brief Records that setup finished (or was skipped).
 * @return true when saved.
 */
bool MarkSetupCompleted();

/**
 * @brief Makes the assistant run again at the next start.
 */
void ResetSetup();

/**
 * @brief Folder that photos, videos and voice memos go to.
 *
 * The SD card when there is one, otherwise the phone's internal memory, so
 * MINI (or FULL with the card out) can still take a photo.
 *
 * @param storage Storage provider; may be null.
 * @param mount_card Try to mount the card first (slow when none is inserted).
 * @param on_card Set to whether the path is on the SD card; may be null.
 * @return Base path, or nullptr when neither is usable.
 */
const char* MediaBasePath(
    hal::StorageProvider* storage, bool mount_card, bool* on_card);

/**
 * @brief Whether internal memory still has room for one more media file.
 *
 * Internal memory is about 4 MB and also holds every setting, so media may
 * not eat into the last part of it.
 *
 * @param bytes_needed Expected size of the file.
 * @return true when the file fits above the reserve.
 */
bool InternalMediaHasRoom(size_t bytes_needed);

}  // namespace lilygo_box::app
