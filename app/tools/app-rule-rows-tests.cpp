// Executable spec for the App split rules sheet's list (App/AppRuleRows.h):
// one row per app, however many rules name it and in whatever case, so the
// sheet's reconcile never meets a row twice - run against the SAME header the
// app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App app-rule-rows-tests.cpp -o /tmp/app-rule-rows-tests && /tmp/app-rule-rows-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cctype>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AppRuleRows.h"

namespace {

// The shapes the sheet passes: InstalledApp and AppRule (InstalledApps.h,
// SdkHost.h), which need Windows headers.
struct App {
  std::string name;
  std::string exePath;
};
struct Rule {
  std::string imagePath;
  bool includeInTunnel = true;
};

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

// An element is its identity, as a UIElement is: two rows are the same row
// only when they are the same object.
using Element = std::shared_ptr<const std::string>;
Element MakeElement(std::string label) { return std::make_shared<const std::string>(std::move(label)); }
bool Same(Element const& a, Element const& b) { return a.get() == b.get(); }

// A panel's UIElementCollection, with WinRT's bounds: GetAt and RemoveAt take
// an index below Size(), InsertAt one up to Size(); anything else is E_BOUNDS.
struct FakeChildren {
  std::vector<Element> items;
  uint32_t Size() const { return static_cast<uint32_t>(items.size()); }
  Element GetAt(uint32_t index) const {
    if (index >= items.size()) throw std::out_of_range("E_BOUNDS: GetAt(" + std::to_string(index) + ")");
    return items[index];
  }
  void RemoveAt(uint32_t index) {
    if (index >= items.size()) throw std::out_of_range("E_BOUNDS: RemoveAt(" + std::to_string(index) + ")");
    items.erase(items.begin() + index);
  }
  void InsertAt(uint32_t index, Element const& element) {
    if (index > items.size()) {
      throw std::out_of_range("E_BOUNDS: InsertAt(" + std::to_string(index) + ") into " +
                              std::to_string(items.size()) + " children");
    }
    items.insert(items.begin() + index, element);
  }
  bool IndexOf(Element const& element, uint32_t& index) const {
    for (uint32_t i = 0; i < items.size(); ++i) {
      if (Same(items[i], element)) {
        index = i;
        return true;
      }
    }
    return false;
  }
};

// The sheet around the list, as AppRulesSheet::RenderList builds it: its rows
// cached by RowKey for the sheet's life, the two group headers and the two
// empty-state lines built once, and the panel's children reconciled to the
// wanted list on every render.
struct Sheet {
  std::vector<App> installed;
  std::unordered_map<std::string, Element> rows;
  Element configuredHeader = MakeElement("header: Configured");
  Element appsHeader = MakeElement("header: Apps");
  Element searchEmptyLine = MakeElement("line: no apps found");
  Element noAppsLine = MakeElement("line: no installed apps found");
  FakeChildren children;

  Element RowFor(App const& app) {
    const std::string key = urnw::apprules::RowKey(app.exePath);
    if (auto it = rows.find(key); it != rows.end()) return it->second;
    Element row = MakeElement("row: " + key);
    rows.emplace(key, row);
    return row;
  }

  std::vector<Element> Want(std::vector<Rule> const& rules, std::string const& query) {
    const std::vector<App> ruled = urnw::apprules::RuledApps(rules, installed);
    const std::vector<App> unruled = urnw::apprules::UnruledApps(rules, installed);
    auto matches = [&](App const& app) {
      return query.empty() ||
             urnw::apprules::RowKey(app.name).find(query) != std::string::npos ||
             urnw::apprules::RowKey(app.exePath).find(query) != std::string::npos;
    };
    std::vector<Element> want;
    if (ruled.empty() && unruled.empty()) {
      want.push_back(noAppsLine);
      return want;
    }
    std::vector<Element> ruledShown, unruledShown;
    for (const auto& app : ruled) {
      if (matches(app)) ruledShown.push_back(RowFor(app));
    }
    for (const auto& app : unruled) {
      if (matches(app)) unruledShown.push_back(RowFor(app));
    }
    if (!ruledShown.empty()) {
      want.push_back(configuredHeader);
      want.insert(want.end(), ruledShown.begin(), ruledShown.end());
    }
    if (!unruledShown.empty()) {
      want.push_back(appsHeader);
      want.insert(want.end(), unruledShown.begin(), unruledShown.end());
    }
    if (want.empty()) want.push_back(searchEmptyLine);
    return want;
  }

  // One render. Returns false (and says why) when the reconcile threw or left
  // the children anything but the wanted list.
  bool Render(std::vector<Rule> const& rules, std::string const& query, std::string const& what) {
    const std::vector<Element> want = Want(rules, query);
    try {
      urnw::apprules::Reconcile(children, want, Same);
    } catch (std::exception const& error) {
      Fail(what + ": the reconcile threw " + error.what());
      return false;
    }
    if (children.items.size() != want.size()) {
      Fail(what + ": " + std::to_string(children.items.size()) + " children, want " +
           std::to_string(want.size()));
      return false;
    }
    for (size_t i = 0; i < want.size(); ++i) {
      if (!Same(children.items[i], want[i])) {
        Fail(what + ": child " + std::to_string(i) + " is " + *children.items[i] + ", want " +
             *want[i]);
        return false;
      }
    }
    return true;
  }
};

std::vector<std::string> Keys(std::vector<App> const& apps) {
  std::vector<std::string> keys;
  for (const auto& app : apps) keys.push_back(urnw::apprules::RowKey(app.exePath));
  return keys;
}

std::string Join(std::vector<std::string> const& parts) {
  std::string out;
  for (const auto& part : parts) out += (out.empty() ? "" : " | ") + part;
  return "[" + out + "]";
}

void TwoRulesForOneApp() {
  TEST_CASE("two rules whose paths differ only in case are one app's: one row");
  const std::vector<App> installed{{"Tool", "C:\\Tools\\Tool.exe"}, {"Other", "C:\\Other\\other.exe"}};
  const std::vector<Rule> rules{{"C:\\Tools\\Tool.exe", false}, {"c:\\tools\\TOOL.EXE", true}};
  const auto ruled = urnw::apprules::RuledApps(rules, installed);
  if (ruled.size() != 1) Fail("ruled rows " + Join(Keys(ruled)) + ", want one");
  if (!ruled.empty() && ruled[0].name != "Tool") Fail("the row is named " + ruled[0].name);
  const auto unruled = urnw::apprules::UnruledApps(rules, installed);
  if (Keys(unruled) != std::vector<std::string>{"c:\\other\\other.exe"}) {
    Fail("unruled rows " + Join(Keys(unruled)) + ", want only the other app");
  }
}

void TheSheetOpensWithThatPair() {
  TEST_CASE("the sheet's first render with that pair reconciles (it threw E_BOUNDS)");
  Sheet sheet;
  sheet.installed = {{"Tool", "C:\\Tools\\Tool.exe"}};
  const std::vector<Rule> rules{{"C:\\Tools\\Tool.exe", false}, {"c:\\tools\\tool.exe", true}};
  sheet.Render(rules, "", "first render");
  // and a search keystroke, which re-renders the same list
  sheet.Render(rules, "tool", "search \"tool\"");
}

void ARuleTwiceForAnAppNotInstalled() {
  TEST_CASE("a rule twice, in two cases, for an app not installed: one row by file name");
  const std::vector<App> installed;
  const std::vector<Rule> rules{{"D:\\Portable\\Thing.exe", true}, {"d:\\portable\\thing.exe", true}};
  const auto ruled = urnw::apprules::RuledApps(rules, installed);
  if (ruled.size() != 1) {
    Fail("ruled rows " + Join(Keys(ruled)) + ", want one");
    return;
  }
  if (ruled[0].name != "Thing.exe") Fail("the row is named " + ruled[0].name + ", want Thing.exe");
  if (ruled[0].exePath != "D:\\Portable\\Thing.exe") Fail("the row's path is " + ruled[0].exePath);
}

void TheFirstRuleKeepsItsPlace() {
  TEST_CASE("the row keeps the first rule's place in rule order");
  const std::vector<App> installed{{"A", "C:\\a.exe"}, {"B", "C:\\b.exe"}, {"C", "C:\\c.exe"}};
  const std::vector<Rule> rules{{"C:\\b.exe"}, {"C:\\a.exe"}, {"c:\\B.EXE"}, {"C:\\c.exe"}};
  const auto keys = Keys(urnw::apprules::RuledApps(rules, installed));
  const std::vector<std::string> want{"c:\\b.exe", "c:\\a.exe", "c:\\c.exe"};
  if (keys != want) Fail("ruled rows " + Join(keys) + ", want " + Join(want));
}

void OneRowPerInstalledApp() {
  TEST_CASE("an installed list that names one app twice still gets one row");
  const std::vector<App> installed{{"Tool", "C:\\Tools\\Tool.exe"}, {"Tool (x86)", "c:\\tools\\tool.exe"}};
  const auto unruled = urnw::apprules::UnruledApps(std::vector<Rule>{}, installed);
  if (unruled.size() != 1) Fail("unruled rows " + Join(Keys(unruled)) + ", want one");
}

void TheReconcileNeedsEachRowOnce() {
  TEST_CASE("control: the reconcile itself throws E_BOUNDS on a row wanted twice");
  FakeChildren children;
  const Element header = MakeElement("header");
  const Element row = MakeElement("row");
  bool threw = false;
  try {
    urnw::apprules::Reconcile(children, std::vector<Element>{header, row, row}, Same);
  } catch (std::out_of_range const&) {
    threw = true;
  }
  if (!threw) Fail("a want list holding one row twice reconciled without E_BOUNDS");
}

// Every render of every sheet, over seeded random rules, installs and searches
// whose paths come in several cases: each row wanted once, the reconcile never
// throws, and the children end up exactly the wanted list.
void EveryRenderReconciles() {
  TEST_CASE("every render reconciles to exactly the wanted rows (seeded, 300 sheets)");
  uint64_t state = 0x9E3779B97F4A7C15ull;
  auto next = [&](uint32_t bound) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>((state >> 33) % bound);
  };
  const std::vector<std::string> pool{"C:\\Tools\\Tool.exe", "C:\\Program Files\\Browser\\browser.exe",
                                      "D:\\Games\\Game.exe", "C:\\Apps\\Chat.exe",
                                      "C:\\Apps\\Mail.exe", "E:\\Portable\\Thing.exe"};
  auto anyCase = [&](std::string path) {
    for (auto& c : path) {
      if (next(2)) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
  };
  const std::vector<std::string> queries{"", "", "t", "tool", "apps", "exe", "zzz", "c:\\"};
  int renders = 0;
  const int failuresBefore = gFailures;
  for (int sheetIndex = 0; sheetIndex < 300 && gFailures == failuresBefore; ++sheetIndex) {
    Sheet sheet;
    // as EnumerateInstalledApps leaves it: one entry per lowercased path
    std::unordered_set<std::string> seen;
    for (const auto& path : pool) {
      if (next(3) == 0) continue;
      const std::string variant = anyCase(path);
      if (seen.insert(urnw::apprules::RowKey(variant)).second) {
        sheet.installed.push_back({"app" + std::to_string(next(100)), variant});
      }
    }
    std::vector<Rule> rules;
    for (int step = 0; step < 6 && gFailures == failuresBefore; ++step) {
      // the rules change between renders, as a combo change re-renders on the
      // next keystroke: a rule added in some case, or the first one removed
      if (next(3) != 0) rules.push_back({anyCase(pool[next(static_cast<uint32_t>(pool.size()))]), next(2) == 0});
      else if (!rules.empty()) rules.erase(rules.begin());
      const std::string query = queries[next(static_cast<uint32_t>(queries.size()))];
      const std::vector<Element> want = sheet.Want(rules, query);
      std::unordered_set<const std::string*> distinct;
      for (const auto& element : want) distinct.insert(element.get());
      if (distinct.size() != want.size()) {
        Fail("sheet " + std::to_string(sheetIndex) + " render " + std::to_string(step) +
             ": the wanted list holds a row twice");
        break;
      }
      sheet.Render(rules, query, "sheet " + std::to_string(sheetIndex) + " render " + std::to_string(step));
      ++renders;
    }
  }
  std::cout << "  " << renders << " renders reconciled\n";
}

}  // namespace

int main() {
  TwoRulesForOneApp();
  TheSheetOpensWithThatPair();
  ARuleTwiceForAnAppNotInstalled();
  TheFirstRuleKeepsItsPlace();
  OneRowPerInstalledApp();
  TheReconcileNeedsEachRowOnce();
  EveryRenderReconciles();
  std::cout << "app-rule-rows-tests: " << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
