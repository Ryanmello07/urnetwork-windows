// Executable spec for the service's restart-on-failure access (Service/
// InstallVerb.h): the handle the self-restart opens before re-applying the
// failure actions must carry SERVICE_START whenever an action is a restart,
// or ChangeServiceConfig2W refuses the write and the self-restart never arms.
// Runs against the SAME header the service compiles, on any C++20 host.
//
//   c++ -std=c++20 -I ../src/Service install-verb-tests.cpp \
//       -o /tmp/install-verb-tests && /tmp/install-verb-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "InstallVerb.h"

using namespace urnw::install;

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

}  // namespace

int main() {
  // winsvc.h values (main.cpp static_asserts the same pair)
  Check(kServiceChangeConfigAccess == 0x0002, "SERVICE_CHANGE_CONFIG value");
  Check(kServiceStartAccess == 0x0010, "SERVICE_START value");

  Check(RestartsIndefinitely(), "the last failure action restarts");
  Check(AnyFailureActionRestarts(), "a failure action restarts");
  Check((FailureActionsAccess() & kServiceChangeConfigAccess) != 0,
        "failure-action access includes SERVICE_CHANGE_CONFIG");
  Check((FailureActionsAccess() & kServiceStartAccess) != 0,
        "failure-action access includes SERVICE_START, which "
        "ChangeServiceConfig2W requires for SC_ACTION_RESTART");

  std::cout << (gCases - gFailures) << "/" << gCases << " passed\n";
  return gFailures == 0 ? 0 : 1;
}
