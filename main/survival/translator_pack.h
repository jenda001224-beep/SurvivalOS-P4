/*
 * @Description: Reader for the Translator's dictionary bundles (.svtr)
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/split_file.h"

namespace lilygo_box::survival {

// Layout written by tools/prepare_translator.py; see the comment there.
inline constexpr size_t kTranslatorBlockBytes = 4096;
inline constexpr size_t kTranslatorBlockKeyBytes = 32;
inline constexpr int kTranslatorMaxConcepts = 40;

struct TranslatorLanguage {
  char code[12] = {};
  char name[36] = {};
  // eSpeak NG voice, empty when the language cannot be spoken.
  char voice[16] = {};
  uint32_t entry_count = 0;
  uint32_t block_count = 0;
  uint64_t blocks_offset = 0;
  uint64_t keys_offset = 0;
  uint64_t map_offset = 0;
  uint32_t map_count = 0;
};

// One word of one language.
struct TranslatorRecord {
  char key[84] = {};
  char word[124] = {};
  char roman[124] = {};
  // English meanings separated by '\x1f'.
  char glosses[1404] = {};
  // The dictionary form when this word is an inflected form.
  char lemma[124] = {};
  uint8_t part_of_speech = 0;
  uint8_t concept_count = 0;
  uint32_t concepts[kTranslatorMaxConcepts] = {};
  // Where the record sits in its language's block area.
  uint32_t offset = 0;
};

struct TranslatorPhrase {
  uint32_t concept_id = 0;
  uint16_t topic = 0;
};

/**
 * @brief Name of a part of speech code ("noun", "verb", ...).
 */
const char* TranslatorPartOfSpeechName(uint8_t code);

class TranslatorPack {
 public:
  TranslatorPack() = default;
  ~TranslatorPack();
  TranslatorPack(const TranslatorPack&) = delete;
  TranslatorPack& operator=(const TranslatorPack&) = delete;

  /**
   * @brief Opens a bundle (split parts work too) and loads its directory,
   *        phrase list and fold table into PSRAM.
   */
  bool Open(const char* path);
  void Close();
  bool IsOpen() const { return file_.IsOpen(); }

  int LanguageCount() const { return static_cast<int>(language_count_); }
  const TranslatorLanguage& Language(int index) const { return languages_[index]; }
  /**
   * @brief Index of a language code, or -1.
   */
  int FindLanguage(const char* code) const;

  /**
   * @brief Folds text the way the bundle's keys were folded.
   * @return Length written.
   */
  size_t Fold(const char* text, char* out, size_t size) const;

  /**
   * @brief Finds the words of a language whose folded form equals or starts
   *        with the folded query.
   * @param language Language index.
   * @param query Text as typed.
   * @param prefix true for "starts with", false for exact matches only.
   * @param out Records found, best first.
   * @param capacity Size of out.
   * @return Number of records.
   */
  int Find(int language, const char* query, bool prefix,
      TranslatorRecord* out, int capacity);

  /**
   * @brief Reads one record by its offset in the language's block area.
   */
  bool ReadRecord(int language, uint32_t offset, TranslatorRecord* out);

  /**
   * @brief Offsets of the words a language has for one concept.
   * @return Number found.
   */
  int ConceptWords(int language, uint32_t concept_id, uint32_t* offsets, int capacity);

  /**
   * @brief The English word and sense a concept stands for.
   */
  bool Concept(uint32_t concept_id, char* word, size_t word_size, char* sense,
      size_t sense_size);

  uint32_t ConceptCount() const { return concept_count_; }
  int PhraseCount() const { return static_cast<int>(phrase_count_); }
  const TranslatorPhrase& Phrase(int index) const { return phrases_[index]; }
  int TopicCount() const { return topic_count_; }
  const char* Topic(int index) const;

  /**
   * @brief Unpacks the files the bundle carries — eSpeak NG's data in
   *        espeak-ng-data/ and the fonts in fonts/ — unless a marker says
   *        this bundle's files are already there.
   * @param directory Destination ("/sdcard/translator").
   * @return true when the files are in place.
   */
  bool ExtractResources(const char* directory);

 private:
  bool LoadKeys(int language);
  bool ReadBlock(int language, uint32_t block);
  int LowerBlock(int language, const char* key) const;
  bool DecodeRecord(const uint8_t* data, size_t size, size_t* used,
      TranslatorRecord* out) const;
  uint32_t FoldCodepoint(uint32_t code, const char** replacement) const;

  SplitFile file_;
  TranslatorLanguage* languages_ = nullptr;
  uint32_t language_count_ = 0;
  uint32_t concept_count_ = 0;
  uint64_t concepts_offset_ = 0;
  uint64_t concept_text_offset_ = 0;
  uint64_t speech_offset_ = 0;
  uint64_t speech_bytes_ = 0;
  TranslatorPhrase* phrases_ = nullptr;
  uint32_t phrase_count_ = 0;
  char* topic_text_ = nullptr;
  uint16_t* topic_offsets_ = nullptr;
  int topic_count_ = 0;
  // Fold table: sorted code points, offsets into fold_pool_.
  uint32_t* fold_codes_ = nullptr;
  uint32_t* fold_offsets_ = nullptr;
  char* fold_pool_ = nullptr;
  uint32_t fold_count_ = 0;
  // Block keys of the two languages in use (source and target).
  struct KeyCache {
    int language = -1;
    char* keys = nullptr;
  } key_cache_[2];
  int next_key_slot_ = 0;
  // Last block read.
  uint8_t* block_ = nullptr;
  int block_language_ = -1;
  uint32_t block_index_ = UINT32_MAX;
};

}  // namespace lilygo_box::survival
