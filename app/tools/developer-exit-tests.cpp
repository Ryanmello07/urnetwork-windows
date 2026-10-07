// Executable spec for the developer page's exit policy part
// (App/DeveloperExitPresentation.h): the built-in security rules generation of
// each exit's provider, which the exit's State cell shows once the provider's
// diagnostics arrive: the number, "unknown" for a provider that reports its
// policy without one, and nothing before its first diagnostics.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App developer-exit-tests.cpp -o /tmp/developer-exit-tests && /tmp/developer-exit-tests
//
// With URNW_DEVELOPER_EXIT_TESTS_SDK the same cases run on the generated
// header's own urnet::Exit, and an exit parsed through the header's json
// conversions carries the generation under the name the sdk writes. The header
// needs nlohmann/json; both are system includes because the generated code
// does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_DEVELOPER_EXIT_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> developer-exit-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "DeveloperExitPresentation.h"

#if defined(URNW_DEVELOPER_EXIT_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace de = urnw::developerexit;

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

// urnet::Exit, reduced to the fields the decision reads
struct MirrorExit {
  bool ProviderDiagnosticsAvailable = false;
  int64_t ProviderSecurityPolicyGeneration = 0;
};

// What the State cell shows for an exit's policy: "" for no part, "unknown"
// for "policy generation unknown", or the generation's number.
template <typename Exit>
std::string Shown(const Exit& exit) {
  const std::optional<de::PolicyGeneration> policy = de::PolicyGenerationOf(exit);
  if (!policy) return "";
  if (!policy->generation) return "unknown";
  return std::to_string(*policy->generation);
}

template <typename Exit>
void CheckPolicyGenerations(const std::string& label) {
  struct Case {
    bool providerDiagnosticsAvailable;
    int64_t providerSecurityPolicyGeneration;
    std::string want;
    std::string what;
  };
  const std::vector<Case> cases = {
      {false, 0, "", "no diagnostics yet: nothing is known about the policy"},
      {false, 2, "", "no diagnostics yet, whatever the generation field holds"},
      {true, 0, "unknown", "a provider that reports its policy without a generation"},
      {true, -1, "unknown", "a negative generation, which the sdk never sends"},
      {true, 1, "1", "the first generation"},
      {true, 2, "2", "a later generation"},
      {true, std::numeric_limits<int64_t>::max(), "9223372036854775807",
       "the generation the sdk saturates a wire value past int64 to"},
  };
  for (const Case& c : cases) {
    Exit exit{};
    exit.ProviderDiagnosticsAvailable = c.providerDiagnosticsAvailable;
    exit.ProviderSecurityPolicyGeneration = c.providerSecurityPolicyGeneration;
    const std::string got = Shown(exit);
    Check(got == c.want, label + c.what + ": want \"" + c.want + "\", got \"" + got + "\"");
  }
}

}  // namespace

int main() {
  CheckPolicyGenerations<MirrorExit>("");

#if defined(URNW_DEVELOPER_EXIT_TESTS_SDK)
  CheckPolicyGenerations<urnet::Exit>("sdk: ");
  {
    // urnet::Exit from json, through the header's own from_json: the
    // generation crosses the C ABI under the name the sdk writes
    const urnet::Exit exit = nlohmann::json::parse(R"({
      "ClientId": "018f2b6e-3c4d-7a8b-9c0d-1e2f3a4b5c6d",
      "ProviderDiagnosticsAvailable": true,
      "ProviderSecurityPolicyGeneration": 2
    })").get<urnet::Exit>();
    Check(Shown(exit) == "2", "sdk: the generation parsed from an exit's json");
    // a json without the field never reads as a generation
    const urnet::Exit without =
        nlohmann::json::parse(R"({"ProviderDiagnosticsAvailable": true})").get<urnet::Exit>();
    Check(Shown(without) == "unknown", "sdk: an exit's json without the generation reads unknown");
  }
#endif

  std::cout << (gCases - gFailures) << "/" << gCases << " developer exit checks passed"
#if defined(URNW_DEVELOPER_EXIT_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << "\n";
  return gFailures == 0 ? 0 : 1;
}
