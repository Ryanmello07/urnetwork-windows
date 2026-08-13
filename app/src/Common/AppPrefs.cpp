// SPDX-License-Identifier: MPL-2.0
#include "AppPrefs.h"

#include <fstream>

#include "Paths.h"

namespace urnw {

nlohmann::json LoadAppPrefs() {
  std::ifstream f(AppPrefsFile());
  if (!f) return nlohmann::json::object();
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    if (j.is_object()) return j;
  } catch (...) {
  }
  return nlohmann::json::object();
}

void SaveAppPref(const char* key, const nlohmann::json& value) {
  nlohmann::json j = LoadAppPrefs();
  j[key] = value;
  std::ofstream f(AppPrefsFile(), std::ios::trunc);
  if (f) f << j.dump();
}

}  // namespace urnw
