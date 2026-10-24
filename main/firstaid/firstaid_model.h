/*
 * @Description: First Aid: the medicine cabinet, the people it is for, dose
 *               calculation and search
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "firstaid/firstaid_content.h"

namespace lilygo_box::firstaid {

struct CabinetItem {
  std::string medicine;   // Medicine::key, empty for supplies and own items
  std::string name;       // as on the box
  Form form = Form::kTablet;
  float strength_mg = 0;  // per tablet/capsule, or per ml
  float quantity = 0;     // tablets, ml, pieces ...
  int expiry_year = 0;    // 0 = not given
  int expiry_month = 0;
  std::string note;
};

struct Person {
  std::string name;
  float age_years = 30;
  float weight_kg = 70;
  float height_cm = 175;
};

struct DoseAdvice {
  bool allowed = true;
  bool has_numbers = false;
  float dose_mg = 0;
  float units = 0;          // tablets or ml for one dose
  float max_day_mg = 0;
  int interval_hours = 0;
  std::string text;         // the advice in words
};

// Loads and saves the cabinet in internal memory (works without an SD card).
class Cabinet {
 public:
  bool Load();
  bool Save() const;

  std::vector<CabinetItem>& items() { return items_; }
  std::vector<Person>& people() { return people_; }
  int selected_person() const { return selected_person_; }
  void set_selected_person(int index) { selected_person_ = index; }
  const Person& Current() const;

  // Items in the cabinet holding one of the given medicine keys.
  std::vector<size_t> ItemsFor(const char* medicine_keys) const;

 private:
  std::vector<CabinetItem> items_;
  std::vector<Person> people_;
  int selected_person_ = 0;
};

/**
 * @brief The dose of a medicine for a person, from the leaflet rules.
 * @param medicine Medicine.
 * @param person Who takes it.
 * @param item The box in the cabinet (strength), or nullptr.
 */
DoseAdvice CalculateDose(const Medicine& medicine, const Person& person,
    const CabinetItem* item);

/**
 * @brief Body mass index and body surface area (Mosteller), for the person page.
 */
float BodyMassIndex(const Person& person);
float BodySurfaceArea(const Person& person);

/**
 * @brief Lower case, without accents, for matching typed words.
 */
std::string Fold(const char* text);

/**
 * @brief Topics matching the typed words, best first.
 * @param query What was typed ("kašel", "krev z nosu", ...).
 * @param limit How many.
 * @return Topic indices.
 */
std::vector<size_t> SearchTopics(const char* query, size_t limit);

/**
 * @brief Whether a cabinet item is past its date, or close to it.
 * @return 0 fine, 1 expires within two months, 2 expired.
 */
int ExpiryState(const CabinetItem& item, int year, int month);

}  // namespace lilygo_box::firstaid
