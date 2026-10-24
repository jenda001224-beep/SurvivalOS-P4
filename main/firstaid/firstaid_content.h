/*
 * @Description: First Aid guide content: topics, medicines and the herbs
 *               that help, built into the firmware so it works without a card
 * @License: GPL 3.0
 *
 * The steps follow the European Resuscitation Council guidelines 2021 and the
 * first aid manuals of the Czech Red Cross and the IFRC (2020). Doses are the
 * usual ones from the package leaflets of medicines sold in Czechia. This is a
 * guide for when no help is at hand, not a substitute for a doctor: every
 * screen says when to call 155 / 112.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::firstaid {

enum class Category : uint8_t {
  kEmergency = 0,   // life threatening
  kInjury,          // wounds, fractures, burns
  kBandage,         // how to bandage and splint
  kIllness,         // symptoms and common illness
  kPoison,          // poisoning, bites, stings
  kEnvironment,     // heat, cold, water, lightning
  kCount,
};

const char* CategoryName(Category category);

enum class Urgency : uint8_t {
  kSelfCare = 0,    // can be handled at home
  kDoctor,          // see a doctor soon
  kEmergency,       // call 155 / 112 now
};

// One guide page. Multi-line fields use '\n' between lines.
struct Topic {
  const char* id;
  const char* title;
  Category category;
  Urgency urgency;
  // Words people type: symptoms, synonyms, without diacritics too.
  const char* keywords;
  // When to call 155 / 112 at once.
  const char* call_when;
  // What to do, in order.
  const char* steps;
  // What not to do.
  const char* avoid;
  // Medicine keys (see Medicine::key) that help, space separated.
  const char* medicines;
  // Herbs: "Latin name|Czech name|how to use" per line.
  const char* plants;
  // Topic ids to read next, space separated.
  const char* related;
};

enum class Form : uint8_t {
  kTablet = 0,
  kCapsule,
  kSyrup,       // strength per ml
  kDrops,       // strength per ml
  kCream,
  kSpray,
  kSolution,    // to put on skin or wounds
  kPowder,      // sachet
  kSuppository,
  kBandage,     // dressings, plasters, tapes, splints
  kTool,        // tweezers, scissors, thermometer, tourniquet ...
  kOther,
  kCount,
};

const char* FormName(Form form);

// Dosing rules from the package leaflets. All doses in mg of the active
// ingredient; zero means "not given by weight" or "no such limit".
struct Medicine {
  const char* key;
  const char* name;          // active ingredient, Czech
  const char* brands;        // common Czech brands, for recognising the box
  const char* uses;          // symptom words, for the search
  Form usual_form;
  float usual_strength_mg;   // per tablet, or per ml for liquids
  // Children (and adults under adult_from_kg): per dose and per day, by weight.
  float child_mg_per_kg;
  float child_max_mg_per_kg_day;
  // Adults: per dose and per day.
  float adult_dose_mg;
  float adult_max_mg_day;
  uint8_t interval_hours;    // at least this long between doses
  uint8_t min_age_months;    // not for younger children
  float adult_from_kg;       // adult dosing from this weight (and 12 years)
  const char* dosing;        // the leaflet rule in words
  const char* warnings;      // who must not take it, interactions
};

// Everything a cabinet can hold that is not a medicine has key "" and is
// listed here only for the add-item picker.
struct Supply {
  const char* name;
  Form form;
  const char* uses;
};

size_t TopicCount();
const Topic& TopicAt(size_t index);
const Topic* FindTopic(const char* id);

size_t MedicineCount();
const Medicine& MedicineAt(size_t index);
const Medicine* FindMedicine(const char* key);

size_t SupplyCount();
const Supply& SupplyAt(size_t index);

// What to do when someone ate a poisonous plant or mushroom; the Nature app
// shows it on every species marked poisonous or deadly.
extern const char* const kPoisoningSteps;
extern const char* const kEmergencyNumbers;

}  // namespace lilygo_box::firstaid
