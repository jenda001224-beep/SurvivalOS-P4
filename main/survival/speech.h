/*
 * @Description: Offline text to speech through eSpeak NG
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>

namespace lilygo_box::survival {

/**
 * @brief Speech synthesis with eSpeak NG's formant voices, ~100 languages.
 *
 * eSpeak reads its phoneme tables and pronunciation dictionaries as files, so
 * the data has to sit unpacked in a directory on the card (the Translator
 * unpacks it from its bundle the first time). Synthesis runs on its own task
 * with a large stack in PSRAM and writes a mono 16-bit WAV file, which the
 * normal audio file player then plays.
 */
class Speech {
 public:
  Speech() = default;
  ~Speech();
  Speech(const Speech&) = delete;
  Speech& operator=(const Speech&) = delete;

  /**
   * @brief Loads eSpeak's data. Safe to call again; cheap once loaded.
   * @param data_directory Directory holding phontab, phondata, voices/, ...
   * @return true when synthesis is ready.
   */
  bool Open(const char* data_directory);

  /**
   * @brief Frees everything eSpeak holds.
   */
  void Close();

  bool is_open() const { return open_; }

  /**
   * @brief Speaks text into a WAV file.
   * @param text UTF-8 text.
   * @param voice eSpeak voice or language name ("cs", "de", "cmn", ...).
   * @param wav_path Output file.
   * @param duration_ms Receives the length of the audio; may be null.
   * @return true when the file was written.
   */
  bool SynthesizeToFile(const char* text, const char* voice,
      const char* wav_path, unsigned int* duration_ms);

 private:
  bool open_ = false;
};

}  // namespace lilygo_box::survival
