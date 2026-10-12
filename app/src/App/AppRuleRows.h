// The App split rules sheet's list as data (StatsSheets.cpp,
// AppRulesSheet::RenderList): which apps get a row, in which group and order,
// and how the list's children are brought to those rows without rebuilding
// them. WinRT-free and header-only, so tools/app-rule-rows-tests.cpp runs the
// code the sheet runs.
//
// ONE ROW PER APP. An app is its exe image path without regard to case: Windows
// resolves paths that way, and the WFP driver that applies these rules matches
// them that way (InstalledApps.h). So the sheet caches one row per RowKey, and
// two rules whose paths differ only in case are two rules for one app. The
// ruled list used to hold an entry per RULE, which put that app's one cached
// row in the wanted list twice; Reconcile moved it into place at its first
// position and, at its second, removed it and inserted it past the end of the
// children, which WinRT refuses with E_BOUNDS. On the first render that throw
// reached ShowAppRulesSheet's catch, and the sheet did not open. A rule for an
// app that already has its row now adds none; the row keeps the first rule's
// place, and its state stays the sheet's ruleFor reading.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace urnw::apprules {

// An app's row key: its image path, ASCII-lowercased.
inline std::string RowKey(std::string path) {
  std::transform(path.begin(), path.end(), path.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return path;
}

// The apps with a rule, pinned on top in rule order, one per app (above). A
// rule whose app was not enumerated still gets a row, named by the path's file
// name, so an existing rule stays visible and changeable. App is InstalledApp
// {name, exePath}; Rule is AppRule {imagePath, includeInTunnel}.
template <class App, class Rule>
std::vector<App> RuledApps(std::vector<Rule> const& rules, std::vector<App> const& installed) {
  std::unordered_map<std::string, const App*> installedByKey;
  for (const auto& app : installed) installedByKey[RowKey(app.exePath)] = &app;
  std::unordered_set<std::string> shown;
  std::vector<App> ruled;
  for (const auto& rule : rules) {
    const std::string key = RowKey(rule.imagePath);
    if (!shown.insert(key).second) continue;  // this app has its row already
    if (auto it = installedByKey.find(key); it != installedByKey.end()) {
      ruled.push_back(*it->second);
      continue;
    }
    std::string name = rule.imagePath;
    if (auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
      name = name.substr(slash + 1);
    }
    ruled.push_back(App{name, rule.imagePath});
  }
  return ruled;
}

// The installed apps no rule names, in enumeration order, one per app: the
// enumeration already drops a path it has seen in another case
// (EnumerateInstalledApps), and this holds without relying on it.
template <class App, class Rule>
std::vector<App> UnruledApps(std::vector<Rule> const& rules, std::vector<App> const& installed) {
  std::unordered_set<std::string> shown;
  for (const auto& rule : rules) shown.insert(RowKey(rule.imagePath));
  std::vector<App> unruled;
  for (const auto& app : installed) {
    if (shown.insert(RowKey(app.exePath)).second) unruled.push_back(app);
  }
  return unruled;
}

// Brings a panel's children to `want`, in order, without rebuilding them: drops
// the children `want` does not hold, inserts the ones it lacks and moves the
// survivors into place (the ClientContractsSheet move-in-place idiom), so the
// list's theme transitions animate only the difference. `same` is element
// identity. Children is the panel's UIElementCollection: GetAt and RemoveAt
// take an index below Size(), InsertAt one up to Size(), else E_BOUNDS - which
// is why `want` must hold each element once (one row per app, above).
template <class Children, class Element, class Same>
void Reconcile(Children&& children, std::vector<Element> const& want, Same same) {
  for (uint32_t i = children.Size(); i > 0; --i) {
    const Element child = children.GetAt(i - 1);
    const bool keep = std::any_of(want.begin(), want.end(),
                                  [&](Element const& wanted) { return same(wanted, child); });
    if (!keep) children.RemoveAt(i - 1);
  }
  for (uint32_t i = 0; i < want.size(); ++i) {
    uint32_t cur = 0;
    if (children.IndexOf(want[i], cur)) {
      if (cur != i) {
        children.RemoveAt(cur);
        children.InsertAt(i, want[i]);
      }
    } else {
      children.InsertAt(i, want[i]);
    }
  }
}

}  // namespace urnw::apprules
