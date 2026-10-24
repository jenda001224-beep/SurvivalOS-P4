/*
 * @Description: First Aid: the medicine cabinet, the people it is for, dose
 *               calculation and search
 * @License: GPL 3.0
 */
#include "firstaid/firstaid_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/storage/littlefs_storage.h"
#include "base/logger.h"
#include "survival/nature_pack.h"

namespace lilygo_box::firstaid {
namespace {

constexpr const char* kCabinetFile = "firstaid.txt";

std::string CabinetPath() {
  const char* base = app::IsLittleFsStorageMounted() ? app::LittleFsStorageBasePath() : nullptr;
  if (base == nullptr) {
    return std::string();
  }
  return std::string(base) + "/" + kCabinetFile;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) {
      break;
    }
    start = tab + 1;
  }
  return parts;
}

std::string Clean(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return out;
}

// Formats 1.5 as "1,5" and 2.0 as "2".
std::string Number(float value) {
  char buffer[24];
  if (std::fabs(value - std::round(value)) < 0.05f) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    for (char* p = buffer; *p; ++p) {
      if (*p == '.') *p = ',';
    }
  }
  return buffer;
}

const char* UnitWord(Form form, float units) {
  // Czech: 1 tableta, 2–4 tablety, 5 tablet, and halves take "tablety".
  const bool whole = std::fabs(units - std::round(units)) < 0.01f;
  const bool one = whole && units <= 1.0f;
  if (!whole) {
    units = 2;
  }
  switch (form) {
    case Form::kTablet: return one ? "tableta" : (units < 5 ? "tablety" : "tablet");
    case Form::kCapsule: return one ? "tobolka" : (units < 5 ? "tobolky" : "tobolek");
    case Form::kSyrup:
    case Form::kDrops: return "ml";
    case Form::kSuppository: return one ? "čípek" : (units < 5 ? "čípky" : "čípků");
    case Form::kPowder: return one ? "sáček" : "sáčky";
    default: return "ks";
  }
}

// Leaflet dosing by age where the leaflet gives no rule per kilogram.
bool ChildDoseByAge(const Medicine& medicine, const Person& person, float* dose,
    float* max_day, int* interval) {
  const float age = person.age_years;
  const std::string key = medicine.key;
  if (key == "cetirizine") {
    if (age >= 6) { *dose = 5; *max_day = 10; *interval = 12; return true; }
    if (age >= 2) { *dose = 2.5f; *max_day = 5; *interval = 12; return true; }
  } else if (key == "loratadine") {
    if (age >= 2) { *dose = person.weight_kg > 30 ? 10 : 5; *max_day = *dose; *interval = 24; return true; }
  } else if (key == "ambroxol") {
    if (age >= 6) { *dose = 15; *max_day = 45; *interval = 8; return true; }
    if (age >= 2) { *dose = 7.5f; *max_day = 22.5f; *interval = 8; return true; }
  } else if (key == "acetylcysteine") {
    if (age >= 6) { *dose = 100; *max_day = 300; *interval = 8; return true; }
    if (age >= 2) { *dose = 100; *max_day = 300; *interval = 8; return true; }
  } else if (key == "activated_charcoal") {
    if (age >= 2) { *dose = 250; *max_day = 750; *interval = 8; return true; }
  } else if (key == "simethicone") {
    if (age >= 6) { *dose = 40; *max_day = 200; *interval = 4; return true; }
  } else if (key == "epinephrine") {
    if (person.weight_kg >= 15) { *dose = person.weight_kg >= 30 ? 0.3f : 0.15f; *max_day = 0; *interval = 0; return true; }
  }
  return false;
}

}  // namespace

// ── Cabinet ─────────────────────────────────────────────────────────────────

bool Cabinet::Load() {
  items_.clear();
  people_.clear();
  selected_person_ = 0;
  const std::string path = CabinetPath();
  FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
  if (file != nullptr) {
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file) != nullptr) {
      std::string line(buffer);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      const std::vector<std::string> parts = SplitTabs(line);
      if (parts.size() >= 9 && parts[0] == "item") {
        CabinetItem item;
        item.medicine = parts[1];
        item.name = parts[2];
        item.form = static_cast<Form>(std::min<int>(std::atoi(parts[3].c_str()),
            static_cast<int>(Form::kCount) - 1));
        item.strength_mg = static_cast<float>(std::atof(parts[4].c_str()));
        item.quantity = static_cast<float>(std::atof(parts[5].c_str()));
        item.expiry_year = std::atoi(parts[6].c_str());
        item.expiry_month = std::atoi(parts[7].c_str());
        item.note = parts[8];
        items_.push_back(item);
      } else if (parts.size() >= 5 && parts[0] == "person") {
        Person person;
        person.name = parts[1];
        person.age_years = static_cast<float>(std::atof(parts[2].c_str()));
        person.weight_kg = static_cast<float>(std::atof(parts[3].c_str()));
        person.height_cm = static_cast<float>(std::atof(parts[4].c_str()));
        people_.push_back(person);
      } else if (parts.size() >= 2 && parts[0] == "selected") {
        selected_person_ = std::atoi(parts[1].c_str());
      }
    }
    std::fclose(file);
  }
  if (people_.empty()) {
    Person me;
    me.name = "Já";
    people_.push_back(me);
  }
  selected_person_ = std::clamp(selected_person_, 0, static_cast<int>(people_.size()) - 1);
  return true;
}

bool Cabinet::Save() const {
  const std::string path = CabinetPath();
  if (path.empty()) {
    return false;
  }
  const std::string temporary = path + ".new";
  FILE* file = std::fopen(temporary.c_str(), "w");
  if (file == nullptr) {
    return false;
  }
  for (const Person& person : people_) {
    std::fprintf(file, "person\t%s\t%.1f\t%.1f\t%.0f\n", Clean(person.name).c_str(),
        person.age_years, person.weight_kg, person.height_cm);
  }
  std::fprintf(file, "selected\t%d\n", selected_person_);
  for (const CabinetItem& item : items_) {
    std::fprintf(file, "item\t%s\t%s\t%d\t%.3f\t%.1f\t%d\t%d\t%s\n", Clean(item.medicine).c_str(),
        Clean(item.name).c_str(), static_cast<int>(item.form), item.strength_mg, item.quantity,
        item.expiry_year, item.expiry_month, Clean(item.note).c_str());
  }
  const bool ok = std::fclose(file) == 0;
  if (!ok) {
    std::remove(temporary.c_str());
    return false;
  }
  std::remove(path.c_str());
  return std::rename(temporary.c_str(), path.c_str()) == 0;
}

const Person& Cabinet::Current() const {
  return people_[static_cast<size_t>(std::clamp(selected_person_, 0,
      static_cast<int>(people_.size()) - 1))];
}

std::vector<size_t> Cabinet::ItemsFor(const char* medicine_keys) const {
  std::vector<size_t> found;
  if (medicine_keys == nullptr) {
    return found;
  }
  for (size_t i = 0; i < items_.size(); ++i) {
    const std::string& key = items_[i].medicine;
    if (key.empty()) {
      continue;
    }
    // Whole-word match in the space separated list.
    const char* p = std::strstr(medicine_keys, key.c_str());
    while (p != nullptr) {
      const bool start = p == medicine_keys || p[-1] == ' ';
      const char end = p[key.size()];
      if (start && (end == '\0' || end == ' ')) {
        found.push_back(i);
        break;
      }
      p = std::strstr(p + 1, key.c_str());
    }
  }
  return found;
}

// ── Doses ───────────────────────────────────────────────────────────────────

DoseAdvice CalculateDose(const Medicine& medicine, const Person& person,
    const CabinetItem* item) {
  DoseAdvice advice;
  const float age_months = person.age_years * 12.0f;
  if (medicine.min_age_months > 0 && age_months < medicine.min_age_months) {
    advice.allowed = false;
    const float years = medicine.min_age_months / 12.0f;
    advice.text = years >= 1 ? "Není pro děti mladší " + Number(years) + " let – poraď se s lékařem."
                             : "Není pro kojence mladší " + Number(medicine.min_age_months) +
                                   " měsíců – poraď se s lékařem.";
    return advice;
  }
  const bool adult = person.age_years >= 12 &&
      (medicine.adult_from_kg <= 0 || person.weight_kg >= medicine.adult_from_kg);
  float dose = 0;
  float max_day = 0;
  int interval = medicine.interval_hours;
  if (adult) {
    dose = medicine.adult_dose_mg;
    max_day = medicine.adult_max_mg_day;
  } else if (medicine.child_mg_per_kg > 0 && person.weight_kg > 0) {
    dose = std::min(person.weight_kg * medicine.child_mg_per_kg,
        medicine.adult_dose_mg > 0 ? medicine.adult_dose_mg : 1e9f);
    max_day = std::min(person.weight_kg * medicine.child_max_mg_per_kg_day,
        medicine.adult_max_mg_day > 0 ? medicine.adult_max_mg_day : 1e9f);
  } else if (!ChildDoseByAge(medicine, person, &dose, &max_day, &interval)) {
    dose = 0;
  }
  if (dose <= 0) {
    advice.text = "Dávkuj podle příbalového letáku.";
    return advice;
  }
  advice.has_numbers = true;
  advice.dose_mg = dose;
  advice.max_day_mg = max_day;
  advice.interval_hours = interval;

  std::string text = "Jedna dávka: " + Number(dose) + " mg";
  const float strength = item != nullptr && item->strength_mg > 0 ? item->strength_mg
                                                                   : medicine.usual_strength_mg;
  const Form form = item != nullptr ? item->form : medicine.usual_form;
  if (strength > 0) {
    float units = dose / strength;
    if (form == Form::kTablet || form == Form::kCapsule || form == Form::kSuppository) {
      // Tablets in halves at most, rounded down so the dose is never exceeded.
      const float rounded = std::floor(units * 2.0f + 0.01f) / 2.0f;
      if (rounded < 0.5f || (form != Form::kTablet && rounded < 1.0f)) {
        text += " – méně než " + std::string(form == Form::kTablet ? "půl tablety" : "jeden kus") +
            " (" + Number(strength) + " mg): použij dětskou formu (sirup, čípek).";
        advice.text = text;
        return advice;
      }
      if (form != Form::kTablet) {
        units = std::floor(units + 0.01f);
      } else {
        units = rounded;
      }
      advice.units = units;
      const float actual = units * strength;
      const bool rounded_down = actual < dose - 0.5f;
      text = std::string("Jedna dávka ") + (rounded_down ? "až " : "") + Number(dose) + " mg: ";
      if (std::fabs(units - 0.5f) < 0.01f) {
        text += "půl " + std::string(form == Form::kTablet ? "tablety" : UnitWord(form, 0.5f)) +
            " po " + Number(strength) + " mg";
      } else {
        text += Number(units) + " " + UnitWord(form, units) + " po " + Number(strength) + " mg";
      }
      if (rounded_down) {
        text += " (= " + Number(actual) + " mg)";
      }
    } else if (form == Form::kSyrup || form == Form::kDrops) {
      const float ml = std::floor(units * 2.0f + 0.01f) / 2.0f;
      advice.units = ml;
      text += " = " + Number(ml) + " ml (" + Number(strength) + " mg v 1 ml)";
    }
  }
  if (interval > 0) {
    text += "\nDalší dávka nejdříve za " + std::to_string(interval) + " h";
  }
  if (max_day > 0) {
    text += ", nejvýš " + Number(max_day) + " mg za den";
    if (dose > 0) {
      const int doses = static_cast<int>(std::floor(max_day / dose + 0.01f));
      text += " (" + std::to_string(doses) +
          (doses == 1 ? " dávka)" : doses <= 4 ? " dávky)" : " dávek)");
    }
  }
  text += ".";
  advice.text = text;
  return advice;
}

float BodyMassIndex(const Person& person) {
  const float meters = person.height_cm / 100.0f;
  return meters > 0.3f ? person.weight_kg / (meters * meters) : 0;
}

float BodySurfaceArea(const Person& person) {
  return std::sqrt(std::max(0.0f, person.height_cm * person.weight_kg) / 3600.0f);
}

// ── Search ──────────────────────────────────────────────────────────────────

std::string Fold(const char* text) {
  char buffer[512];
  survival::FoldForSearch(text != nullptr ? text : "", buffer, sizeof(buffer));
  return buffer;
}

// Czech drops an "e" when it bends a word (kašel – kašle, pes – psa), so the
// comparison ignores every "e" that does not start a word.
std::string WithoutMobileE(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == 'e' && i > 0 && text[i - 1] != ' ') {
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

std::vector<size_t> SearchTopics(const char* query, size_t limit) {
  std::vector<std::string> words;
  const std::string folded = Fold(query);
  size_t start = 0;
  while (start < folded.size()) {
    size_t end = folded.find(' ', start);
    if (end == std::string::npos) end = folded.size();
    if (end - start >= 2) {
      words.push_back(folded.substr(start, end - start));
    }
    start = end + 1;
  }
  std::vector<std::pair<int, size_t>> scored;
  if (words.empty()) {
    return {};
  }
  for (size_t i = 0; i < TopicCount(); ++i) {
    const Topic& topic = TopicAt(i);
    const std::string title = WithoutMobileE(" " + Fold(topic.title));
    const std::string keys = WithoutMobileE(" " + Fold(topic.keywords) + " " + Fold(topic.medicines));
    int score = 0;
    int matched = 0;
    for (const std::string& word : words) {
      // Word starts: "kas" finds "kasel"; Czech endings are cut a little so
      // "kašlem" still finds "kašel".
      std::string stem = WithoutMobileE(word.size() > 5 ? word.substr(0, word.size() - 2) : word);
      const bool in_title = title.find(" " + stem) != std::string::npos;
      const bool in_keys = keys.find(" " + stem) != std::string::npos;
      if (in_title || in_keys) {
        ++matched;
        score += in_title ? 3 : 1;
      }
    }
    if (matched == static_cast<int>(words.size())) {
      score += 10;
    }
    // The topic named exactly like the query comes first ("horečka" is
    // Horečka, not Horečnaté křeče).
    const std::string plain_title = WithoutMobileE(" " + Fold(topic.title));
    if (plain_title == WithoutMobileE(" " + folded) ||
        plain_title.rfind(WithoutMobileE(" " + folded) + " ", 0) == 0) {
      score += 20;
    }
    if (matched > 0) {
      scored.push_back({score * 4 + static_cast<int>(topic.urgency), i});
    }
  }
  std::stable_sort(scored.begin(), scored.end(),
      [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<size_t> result;
  for (const auto& entry : scored) {
    if (result.size() >= limit) break;
    result.push_back(entry.second);
  }
  return result;
}

int ExpiryState(const CabinetItem& item, int year, int month) {
  if (item.expiry_year <= 0 || year <= 0) {
    return 0;
  }
  const int now = year * 12 + month;
  const int expires = item.expiry_year * 12 + std::max(1, item.expiry_month);
  if (expires < now) {
    return 2;
  }
  return expires - now <= 2 ? 1 : 0;
}

}  // namespace lilygo_box::firstaid
