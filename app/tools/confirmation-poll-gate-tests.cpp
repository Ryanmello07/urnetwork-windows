// Executable spec for the purchase-confirmation poll gate
// (App/ConfirmationPollGate.h): the give-up budget only burns while the window
// is visible AND focused, so a hosted checkout paid in the browser never comes
// back to a false "timed out" (UPGRADE.md D1).
//
//   c++ -std=c++20 -I ../src/App confirmation-poll-gate-tests.cpp \
//       -o /tmp/confirmation-poll-gate-tests && /tmp/confirmation-poll-gate-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "ConfirmationPollGate.h"

using urnw::ConfirmationPollGate;
using urnw::kConfirmationBudgetMillis;

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

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

constexpr int64_t kSecond = 1000;

}  // namespace

int main() {
  // ---- the report: a hosted checkout keeps the window visible behind the
  // browser while the user types card details for ten minutes ----
  {
    ConfirmationPollGate gate(kConfirmationBudgetMillis);
    gate.SetVisible(true, 0);
    Check(gate.Start(0), "a confirmation started in front of the user runs now");
    // the browser takes focus one second in; the window stays visible
    gate.SetFocused(false, 1 * kSecond);
    Check(!gate.Running(), "focus loss pauses the confirmation poll");
    Check(!gate.ExpiredAt(601 * kSecond), "ten minutes in the browser does not spend the budget");
    CheckEq(kConfirmationBudgetMillis - 1 * kSecond, gate.RemainingAt(601 * kSecond),
            "the budget is banked while unfocused");
    // back to the app: the poll resumes with what was left
    Check(gate.SetFocused(true, 601 * kSecond), "refocus resumes the poll (immediate fetch)");
    CheckEq(kConfirmationBudgetMillis - 1 * kSecond, gate.RemainingAt(601 * kSecond),
            "the resumed budget is the banked remainder");
    Check(!gate.ExpiredAt(601 * kSecond + 118 * kSecond), "the remainder still runs after refocus");
    Check(gate.ExpiredAt(601 * kSecond + 119 * kSecond), "and expires once it is spent");
  }

  // ---- a confirmation started while the browser has focus waits for the
  // window before its clock starts ----
  {
    ConfirmationPollGate gate(kConfirmationBudgetMillis);
    gate.SetVisible(true, 0);
    gate.SetFocused(false, 0);
    Check(!gate.Start(0), "a confirmation started unfocused does not run yet");
    Check(!gate.ExpiredAt(30 * 60 * kSecond), "no budget is spent before the first focus");
    Check(gate.SetFocused(true, 30 * 60 * kSecond), "the first focus starts the poll");
    CheckEq(kConfirmationBudgetMillis, gate.RemainingAt(30 * 60 * kSecond), "with the full budget");
  }

  // ---- hiding pauses as before; both gates must be open ----
  {
    ConfirmationPollGate gate(kConfirmationBudgetMillis);
    gate.SetVisible(true, 0);
    gate.Start(0);
    gate.SetVisible(false, 10 * kSecond);
    Check(!gate.Running(), "hiding pauses the poll");
    Check(!gate.SetFocused(true, 20 * kSecond), "focus while hidden does not resume");
    Check(gate.SetVisible(true, 500 * kSecond), "showing again resumes");
    CheckEq(kConfirmationBudgetMillis - 10 * kSecond, gate.RemainingAt(500 * kSecond),
            "time hidden is not spent");
    Check(!gate.SetVisible(true, 501 * kSecond), "a repeated show is not a second resume");
  }

  // ---- the budget still gives up while in front of the user ----
  {
    ConfirmationPollGate gate(kConfirmationBudgetMillis);
    gate.SetVisible(true, 0);
    gate.Start(0);
    Check(!gate.ExpiredAt(kConfirmationBudgetMillis - 1), "not expired a millisecond early");
    Check(gate.ExpiredAt(kConfirmationBudgetMillis), "expires after the full budget in front");
    gate.Stop();
    Check(!gate.Confirming() && !gate.Running(), "stop ends the confirmation");
    Check(!gate.ExpiredAt(10 * kConfirmationBudgetMillis), "a stopped gate never expires");
    Check(!gate.SetFocused(true, 0) && !gate.SetVisible(true, 0), "a stopped gate never resumes");
  }

  // ---- a fresh start grants the full budget again ----
  {
    ConfirmationPollGate gate(kConfirmationBudgetMillis);
    gate.SetVisible(true, 0);
    gate.Start(0);
    gate.Stop();
    gate.Start(100 * kSecond);
    CheckEq(kConfirmationBudgetMillis, gate.RemainingAt(100 * kSecond), "restart grants the full budget");
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " confirmation poll gate checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
