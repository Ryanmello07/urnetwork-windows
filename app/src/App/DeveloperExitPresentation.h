// What the developer page's exit table decides about an exit's provider
// before it touches a XAML object: the built-in security rules generation the
// provider enforces (connect SecurityPolicyRulesGeneration), shown as the last
// part of the exit's State cell. Connect raises the number with every reviewed
// rules change, so an exit with a lower number than the others runs a provider
// with older rules, which can drop flows this device's rules admit.
//
// It is here, and pure, for the reason ExtenderPresentation.h gives: the
// windows solution has no test project and a WinUI 3 app cannot be built off
// Windows, so tools/developer-exit-tests.cpp verifies the decision on any host
// with a C++20 compiler. The function is a template over urnet::Exit's field
// names, so the tests run it on a plain mirror, and again on the generated
// header's own struct when one is available; DeveloperPage.cpp passes the
// sdk's exits straight in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>

namespace urnw::developerexit {

// The provider's reported policy. generation is nullopt for a provider that
// reports its policy without one: it predates the generation, or it runs a
// custom policy (the State cell reads "policy generation unknown").
struct PolicyGeneration {
  std::optional<int64_t> generation;
};

// nullopt before the provider's first diagnostics, when nothing is known about
// its policy yet. The sdk sends 0 for an unknown generation and never a
// negative one.
template <typename Exit>
std::optional<PolicyGeneration> PolicyGenerationOf(const Exit& exit) {
  if (!exit.ProviderDiagnosticsAvailable) return std::nullopt;
  PolicyGeneration policy;
  if (0 < exit.ProviderSecurityPolicyGeneration) {
    policy.generation = exit.ProviderSecurityPolicyGeneration;
  }
  return policy;
}

}  // namespace urnw::developerexit
