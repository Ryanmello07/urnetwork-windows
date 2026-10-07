// Executable spec for how the tray app ends (Common/AppLifetime.h, owner
// decisions 2026-10-05): the tray menu's Quit, and Windows ending the session
// (a sign-out of Windows, a shutdown), stop the tunnel session and the
// provider-only device in the service, while a WM_CLOSE from outside the app
// and the updater's installer handoff exit and leave the service as it is. Run
// against the same header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common app-lifetime-tests.cpp -o /tmp/app-lifetime-tests && /tmp/app-lifetime-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "AppLifetime.h"

using namespace urnw::lifetime;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

constexpr Ending kEndings[] = {Ending::Quit, Ending::SessionEnd, Ending::CloseRequest,
                               Ending::InstallerHandoff};

void TestQuitStopsTheService() {
  const Plan quit = PlanFor(Ending::Quit);
  Check(quit.stopService,
        "quit: the service ends with no session and no provider-only device");
}

void TestSessionEndStopsTheService() {
  const Plan end = PlanFor(Ending::SessionEnd);
  Check(end.stopService,
        "session end: a sign-out of Windows or a shutdown stops the tunnel and the provider, "
        "as quit does");
  Check(end.stopService == PlanFor(Ending::Quit).stopService,
        "session end: the same stop as quit");
}

void TestOtherEndingsLeaveTheService() {
  Check(!PlanFor(Ending::CloseRequest).stopService,
        "close request: the service keeps what it runs (taskkill, an installer)");
  Check(!PlanFor(Ending::InstallerHandoff).stopService,
        "installer handoff: the service is left to the installer, which stops it");
}

void TestOnlyQuitAndSessionEndStop() {
  // the owner's decisions name the Quit and a Windows sign-out; nothing else
  // may stop the service
  int stopping = 0;
  for (Ending ending : kEndings) {
    if (PlanFor(ending).stopService) ++stopping;
  }
  Check(stopping == 2, "only the tray's Quit and the end of the Windows session stop the "
                       "service (got " + std::to_string(stopping) + " endings)");
}

void TestEveryPlanSaysWhy() {
  for (Ending ending : kEndings) {
    const Plan plan = PlanFor(ending);
    Check(plan.why != nullptr && std::string_view(plan.why).size() > 0,
          std::string("why: ") + ToString(ending) + " explains itself in the log");
  }
}

void TestNames() {
  Check(std::string_view(ToString(Ending::Quit)) == "quit", "names: quit");
  Check(std::string_view(ToString(Ending::SessionEnd)) == "session end", "names: session end");
  Check(std::string_view(ToString(Ending::CloseRequest)) == "close request",
        "names: close request");
  Check(std::string_view(ToString(Ending::InstallerHandoff)) == "installer handoff",
        "names: installer handoff");
}

}  // namespace

int main() {
  TestQuitStopsTheService();
  TestSessionEndStopsTheService();
  TestOtherEndingsLeaveTheService();
  TestOnlyQuitAndSessionEndStop();
  TestEveryPlanSaysWhy();
  TestNames();
  std::cout << (gCases - gFailures) << "/" << gCases << " app lifetime checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
