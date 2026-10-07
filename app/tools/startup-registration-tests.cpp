// Executable spec for "Launch URnetwork on system startup" on Windows
// (Common/StartupRegistration.h, owner decision 2026-10-05: like the macOS
// setting, with its default). Runs the planner against a fake registry: the
// user's Run value and Task Manager's StartupApproved record of it. Run
// against the same headers the app compiles, on any host with a C++20
// compiler.
//
//   c++ -std=c++20 -I ../src/Common startup-registration-tests.cpp -o /tmp/startup-registration-tests && /tmp/startup-registration-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "InstanceHandover.h"
#include "StartupRegistration.h"

using namespace urnw::startup;

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

// An install path with spaces, synthetic.
constexpr std::wstring_view kExe = LR"(C:\Program Files\URnetwork\URnetwork.exe)";
// Where an earlier copy of the app lived, synthetic.
constexpr std::wstring_view kOldExe = LR"(D:\Apps\URnetwork (old)\URnetwork.exe)";

// What Task Manager writes for a startup app it has switched off, and on.
constexpr std::uint8_t kTaskManagerDisabled = 0x03;
constexpr std::uint8_t kTaskManagerEnabled = 0x02;
constexpr std::uint8_t kTaskManagerReEnabled = 0x06;

// The Run value the app writes for this install.
std::wstring Command() { return RunCommand(kExe, urnw::instance::kAutostartArgument); }

// The two registry values, and how many writes reached them.
struct FakeRegistry {
  Registration values;
  int writes = 0;

  void Apply(const Plan& plan) {
    if (plan.writeCommand) {
      values.command = *plan.writeCommand;
      ++writes;
    }
    if (plan.deleteCommand) {
      values.command.reset();
      ++writes;
    }
    if (plan.deleteApproval) {
      values.approval.reset();
      ++writes;
    }
  }
};

void TestTheDefaultIsOff() {
  Check(!kDefaultEnabled, "default: off until the user turns it on, as on macOS");
  FakeRegistry fresh;
  Check(!Enabled(fresh.values), "default: a fresh install shows the toggle off");
  fresh.Apply(PlanRefresh(fresh.values, Command()));
  Check(fresh.writes == 0 && !fresh.values.command,
        "first run: a launch registers nothing by itself");
}

void TestTurningItOnRegistersAnAutostart() {
  FakeRegistry registry;
  registry.Apply(PlanSet(true, registry.values, Command()));
  Check(Enabled(registry.values), "on: the toggle then reads on");
  Check(registry.values.command == Command(),
        "on: the Run value starts this install: the quoted exe, then the argument");
  Check(registry.values.command &&
            urnw::instance::HasArgument(*registry.values.command,
                                        urnw::instance::kAutostartArgument),
        "on: the registration launches with --autostart");
  // a sign-in through it is an autostart: the tray icon only, and during an
  // update an exit without a word (main.cpp shows the notice to the user's
  // launches alone)
  const urnw::instance::LaunchRequest signIn{
      .deepLink = {},
      .autostart = urnw::instance::HasArgument(*registry.values.command,
                                               urnw::instance::kAutostartArgument)};
  Check(urnw::instance::ActionFor(signIn) == urnw::instance::LaunchAction::TrayOnly,
        "sign-in: a launch through the registration shows only the tray icon");
  const int writes = registry.writes;
  registry.Apply(PlanSet(true, registry.values, Command()));
  Check(registry.writes == writes, "on again: an up-to-date registration is left alone");
}

void TestTurningItOffUnregisters() {
  FakeRegistry registry;
  registry.values = Registration{.command = Command(), .approval = kTaskManagerEnabled};
  registry.Apply(PlanSet(false, registry.values, Command()));
  Check(!registry.values.command, "off: the Run value is deleted");
  Check(!registry.values.approval, "off: and Task Manager's record of it");
  Check(!Enabled(registry.values), "off: the toggle then reads off");
  const int writes = registry.writes;
  registry.Apply(PlanSet(false, registry.values, Command()));
  Check(registry.writes == writes, "off again: nothing left to delete");
}

void TestTaskManagerCanSwitchItOff() {
  // switched off in Task Manager: registered, but Windows will not run it
  FakeRegistry registry;
  registry.values = Registration{.command = Command(), .approval = kTaskManagerDisabled};
  Check(!Enabled(registry.values),
        "task manager: a registration switched off there shows as off, as macOS shows a login "
        "item switched off in System Settings");
  Check(Enabled(Registration{.command = Command(), .approval = kTaskManagerEnabled}) &&
            Enabled(Registration{.command = Command(), .approval = kTaskManagerReEnabled}),
        "task manager: a registration switched on there shows as on");
  // the user turns it on here: their latest choice wins
  registry.Apply(PlanSet(true, registry.values, Command()));
  Check(Enabled(registry.values) && !registry.values.approval,
        "task manager: turning it on here clears the 'disabled' Task Manager recorded");
  // a launch leaves a Task Manager 'disabled' alone
  FakeRegistry kept;
  kept.values = Registration{.command = Command(), .approval = kTaskManagerDisabled};
  kept.Apply(PlanRefresh(kept.values, Command()));
  Check(kept.writes == 0 && kept.values.approval == kTaskManagerDisabled,
        "task manager: a launch never overrides the user's choice there");
}

void TestALaunchBringsTheRegistrationUpToDate() {
  // the install moved, or an earlier copy registered itself
  FakeRegistry moved;
  moved.values = Registration{
      .command = RunCommand(kOldExe, urnw::instance::kAutostartArgument), .approval = std::nullopt};
  moved.Apply(PlanRefresh(moved.values, Command()));
  Check(moved.values.command == Command(),
        "refresh: a registration of another path now starts this install");
  // a value written without the argument (by hand, or by anything else)
  FakeRegistry bare;
  bare.values = Registration{.command = L"\"" + std::wstring(kExe) + L"\"",
                             .approval = std::nullopt};
  bare.Apply(PlanRefresh(bare.values, Command()));
  Check(bare.values.command &&
            urnw::instance::HasArgument(*bare.values.command, urnw::instance::kAutostartArgument),
        "refresh: a registration without --autostart gains it, so a sign-in shows only the tray "
        "icon");
  // nothing more to do once it is up to date
  FakeRegistry current;
  current.values = Registration{.command = Command(), .approval = std::nullopt};
  current.Apply(PlanRefresh(current.values, Command()));
  Check(current.writes == 0, "refresh: an up-to-date registration is left alone");
}

void TestTheCommandLine() {
  Check(RunCommand(kExe, L"--autostart") ==
            LR"("C:\Program Files\URnetwork\URnetwork.exe" --autostart)",
        "command: the exe is quoted, so a path with spaces stays one argument");
  Check(PlanSet(true, Registration{}, Command()).writeCommand == Command(),
        "command: turning it on writes exactly the command it is given");
  Check(PlanSet(true, Registration{}, Command()).deleteCommand == false &&
            PlanSet(false, Registration{}, Command()).Empty(),
        "plan: nothing is deleted that is not there");
}

}  // namespace

int main() {
  TestTheDefaultIsOff();
  TestTurningItOnRegistersAnAutostart();
  TestTurningItOffUnregisters();
  TestTaskManagerCanSwitchItOff();
  TestALaunchBringsTheRegistrationUpToDate();
  TestTheCommandLine();
  std::cout << (gCases - gFailures) << "/" << gCases << " startup registration checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
