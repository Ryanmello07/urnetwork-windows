// Executable spec for how the app publishes and applies live stats snapshots:
// in the order their reads began (App/StatsReadOrder.h), so a read that
// finishes late cannot roll the window or the tray back to an older state.
// Run against the same headers the app compiles, on any host with a C++20
// compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App stats-publish-tests.cpp -o /tmp/stats-publish-tests && /tmp/stats-publish-tests
//
// SdkHost, AppController and ConnectPage cannot be built off Windows, so
// tests/stats_publish_test.go reads their source for the wiring.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "StatsReadOrder.h"

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Case(const std::string& name) {
  ++gCases;
  gCurrentCase = name;
}

void Check(bool condition, const std::string& message) {
  if (condition) return;
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

}  // namespace

int main() {
  // ---- the order snapshots are applied in ------------------------------------

  {
    Case("the first snapshot is applied, and so is each one read after it");
    urnw::StatsReadOrder order;
    Check(order.Admit(1), "the first snapshot was dropped");
    Check(order.Admit(2), "the next read was dropped");
    Check(order.Admit(5), "a read after skipped sequences was dropped");
  }
  {
    Case("a read that began before the applied one is dropped when it finishes late");
    urnw::StatsReadOrder order;
    // 2 began first and waited on a getter; 3 began after it and finished first
    Check(order.Admit(3), "the newer read was dropped");
    Check(!order.Admit(2), "the older read rolled the snapshot back");
    Check(order.Admit(4), "a read after the applied one was dropped");
    Check(!order.Admit(3), "a read older than the applied one was applied again");
  }
  {
    Case("the same snapshot twice is applied once");
    urnw::StatsReadOrder order;
    Check(order.Admit(7), "the snapshot was dropped");
    Check(!order.Admit(7), "the same snapshot was applied twice");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
