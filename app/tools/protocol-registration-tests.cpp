// Executable spec for the per-user urnetwork:// registration
// (Common/ProtocolRegistration.h): what a browser sign-in writes under
// HKEY_CURRENT_USER so a link comes back to THIS exe, and what it leaves alone.
// Runs the planner against a fake registry holding the user's key. Run against
// the same header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common protocol-registration-tests.cpp -o /tmp/protocol-registration-tests && /tmp/protocol-registration-tests
//
// Every path and version below is synthetic. The one path that stands for the
// owner's machine (an unmarked key around another URnetwork.exe) is an invented
// one in the same shape.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "ProtocolRegistration.h"

using namespace urnw::protocol;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    // flushed, so a spec that is killed a moment later (a header that was made to refuse what it must take
    // leaves the checks below nothing to read) still says what failed
    std::cout << "  FAIL " << what << std::endl;
  }
}

// This install, synthetic; an install path with spaces, as Program Files has.
constexpr std::wstring_view kExe = LR"(C:\Program Files\URnetwork\URnetwork.exe)";
constexpr std::wstring_view kVersion = L"2026.10.9-18693-beta";

// The user's key and every write that reached it, in order.
struct FakeRegistry {
  UserKey key;
  std::vector<std::wstring> written;  // "<subkey>\<name>" of each write, in order
  int writes = 0;
  // A power cut: the writes stop after this many (-1: never)
  int stopAfter = -1;

  void Apply(const Plan& plan) {
    for (const Write& write : plan.writes) {
      if (stopAfter >= 0 && writes >= stopAfter) return;
      ++writes;
      key.exists = true;
      written.push_back(write.subkey + L"\\" + write.name);
      if (write.subkey.empty() && write.name == kOwnerValue) key.owner = write.value;
      else if (write.subkey.empty() && write.name.empty()) key.description = write.value;
      else if (write.subkey.empty() && write.name == kUrlProtocolValue) key.urlProtocol = true;
      else if (write.subkey == kCommandSubkey && write.name.empty()) key.command = write.value;
    }
  }
};

UserKey Ours(std::wstring_view exe = kExe, std::wstring_view version = kVersion) {
  UserKey k;
  k.exists = true;
  k.description = std::wstring(kDescription);
  k.urlProtocol = true;
  k.command = OpenCommand(exe);
  k.owner = OwnerTag(exe, version);
  return k;
}

// Someone else's: another program starts the scheme.
UserKey Foreign() {
  UserKey k;
  k.exists = true;
  k.description = L"URL:Something Else";
  k.urlProtocol = true;
  k.command = LR"("C:\Tools\Other\other.exe" "%1")";
  return k;
}

// The owner's machine, as measured: unmarked, the MSI's shape, another URnetwork.exe
// (a development build in a checkout, invented here).
UserKey DevMachine() {
  UserKey k;
  k.exists = true;
  k.description = std::wstring(kDescription);
  k.urlProtocol = true;
  k.command = OpenCommand(LR"(D:\work\checkout\app\build\x64\Release\URnetwork.exe)");
  return k;
}

void TestTheCommandIsTheMsisShape() {
  Check(OpenCommand(kExe) == LR"("C:\Program Files\URnetwork\URnetwork.exe" "%1")",
        "command: the MSI's shape (Package.wxs): the quoted exe, a space, a quoted %1");
  Check(OpenCommand(LR"(C:\a.exe)") == LR"("C:\a.exe" "%1")", "command: a path without spaces is quoted as well");
  Check(OwnerTag(kExe, kVersion) == std::wstring(kExe) + L"|" + std::wstring(kVersion),
        "marker: the exe, then the version it was written for");
  Check(kScheme == L"urnetwork" && kClassesKey == L"Software\\Classes\\urnetwork" &&
            kCommandSubkey == L"shell\\open\\command" && kDescription == L"URL:URnetwork Protocol" &&
            kUrlProtocolValue == L"URL Protocol" && kExeName == L"URnetwork.exe",
        "names: the scheme's user key and the MSI's values");
  const std::optional<std::wstring> exe = ExeOfCommand(OpenCommand(kExe));
  Check(exe && *exe == kExe, "command: the exe comes back out of the command");
}

void TestAnAbsentKeyIsCreatedMarkerFirst() {
  FakeRegistry registry;
  const Plan plan = PlanEnsure(registry.key, kExe, kVersion);
  Check(plan.decision == Decision::Create && plan.writes.size() == 4, "ensure: an absent key is created with four writes");
  // each of these reads a write, and a plan that has none (a header that refused this exe) is a failure to report, not a vector to index
  const bool four = plan.writes.size() == 4;
  Check(four && plan.writes.front().subkey.empty() && plan.writes.front().name == kOwnerValue &&
            plan.writes.front().value == OwnerTag(kExe, kVersion),
        "ensure: the owner marker is written first");
  Check(four && plan.writes.back().subkey == kCommandSubkey && plan.writes.back().name.empty() &&
            plan.writes.back().value == OpenCommand(kExe),
        "ensure: the command is written last");
  Check(four && plan.writes[1].name.empty() && plan.writes[1].value == kDescription && plan.writes[2].name == kUrlProtocolValue &&
            plan.writes[2].value.empty(),
        "ensure: the description and the empty URL Protocol value are the MSI's");
  registry.Apply(plan);
  Check(registry.key.exists && registry.key.command == OpenCommand(kExe) && registry.key.owner &&
            registry.key.urlProtocol,
        "ensure: the key then starts this exe");
  const Plan again = PlanEnsure(registry.key, kExe, kVersion);
  Check(again.decision == Decision::Current && again.writes.empty(), "ensure: our own current key is left alone");
  registry.Apply(again);
  Check(registry.writes == 4, "ensure: and applying that writes nothing more");
}

void TestOurOwnStaleKeyIsRewritten() {
  Check(PlanEnsure(Ours(LR"(D:\old\URnetwork.exe)"), kExe, kVersion).decision == Decision::Repoint,
        "ensure: our key for a moved exe is rewritten");
  Check(PlanEnsure(Ours(kExe, L"1.0.0-beta"), kExe, kVersion).decision == Decision::Repoint,
        "ensure: our key from another version is rewritten");
  UserKey halfWritten = Ours();
  halfWritten.urlProtocol = false;
  halfWritten.command.reset();
  const Plan completed = PlanEnsure(halfWritten, kExe, kVersion);
  Check(completed.decision == Decision::Repoint && completed.writes.size() == 4,
        "ensure: a marked but half-written key is completed");
  UserKey markerOnly;
  markerOnly.exists = true;
  markerOnly.owner = OwnerTag(kExe, kVersion);
  Check(PlanEnsure(markerOnly, kExe, kVersion).decision == Decision::Repoint,
        "ensure: a key with only the marker, which is what a power cut after the first write leaves, is completed");
  UserKey editedCommand = Ours();
  editedCommand.command = OpenCommand(LR"(D:\somewhere\else\URnetwork.exe)");
  Check(PlanEnsure(editedCommand, kExe, kVersion).decision == Decision::Repoint,
        "ensure: a marked key whose command was changed by hand is written again");
  UserKey noCommand = Ours();
  noCommand.command.reset();
  Check(PlanEnsure(noCommand, kExe, kVersion).decision == Decision::Repoint,
        "ensure: a marked key whose command is gone is written again");
  UserKey wrongDescription = Ours();
  wrongDescription.description = L"URL:Changed";
  Check(PlanEnsure(wrongDescription, kExe, kVersion).decision == Decision::Repoint,
        "ensure: a marked key whose description was changed is written again");
  UserKey otherCase = Ours();
  otherCase.command = OpenCommand(LR"(c:\program files\urnetwork\urnetwork.exe)");
  Check(PlanEnsure(otherCase, kExe, kVersion).decision == Decision::Current,
        "ensure: a command that differs from ours only in the case of the path starts the same file, and is left alone");
  otherCase = Ours(LR"(c:\program files\urnetwork\urnetwork.exe)", kVersion);
  Check(PlanEnsure(otherCase, kExe, kVersion).decision == Decision::Repoint,
        "ensure: a marker that names a differently spelled path is written again (the tag is compared as written)");

  // a power cut after any of the writes leaves a key that is still ours and still completed next time
  for (int stop = 1; stop <= 3; ++stop) {
    FakeRegistry registry;
    registry.stopAfter = stop;
    registry.Apply(PlanEnsure(registry.key, kExe, kVersion));
    Check(registry.key.exists && registry.key.owner.has_value(),
          "power cut: after write " + std::to_string(stop) + " the key carries the marker");
    const Plan next = PlanEnsure(registry.key, kExe, kVersion);
    Check(next.decision == Decision::Repoint, "power cut: after write " + std::to_string(stop) + " the next attempt completes it");
    Check(PlanRefresh(registry.key, kExe, kVersion).decision == Decision::Repoint,
          "power cut: after write " + std::to_string(stop) + " a launch completes it too");
    registry.stopAfter = -1;
    registry.Apply(next);
    Check(PlanEnsure(registry.key, kExe, kVersion).decision == Decision::Current, "power cut: and then it is current");
  }
}

void TestSomeoneElsesKeyIsLeftAlone() {
  FakeRegistry registry;
  registry.key = Foreign();
  const Plan plan = PlanEnsure(registry.key, kExe, kVersion);
  Check(plan.decision == Decision::RefuseForeign && plan.writes.empty(),
        "ensure: another program's key is refused, with no writes");
  registry.Apply(plan);
  Check(registry.writes == 0 && registry.key.command == Foreign().command, "ensure: and it is as it was");
  Policy policy;
  policy.adoptOtherUrnetworkExe = true;
  Check(PlanEnsure(Foreign(), kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: not even when other URnetwork.exe files are adopted: it is not one");
  UserKey lookalike = Foreign();
  lookalike.command = LR"("C:\Tools\NotURnetwork.exe" "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: a file whose name only ends like ours is another program");
  lookalike.command = LR"("C:\Tools\URnetwork.exe.bat" "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: and so is a file that only starts like ours");
  lookalike.command = LR"("C:\Tools\URnetwork.exe" --open "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: a command that is not the MSI's shape is not adopted by its file name");
  lookalike.command = LR"(C:\Tools\URnetwork.exe "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: an unquoted command is not the MSI's shape");
  lookalike.command = LR"("\\server\share\URnetwork.exe" "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: a program on another machine is not adopted by its file name");
  lookalike.command = LR"("C:\a%b\URnetwork.exe" "%1")";
  Check(PlanEnsure(lookalike, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "ensure: a path that could not be registered is not adopted");
  UserKey emptyCommand = Foreign();
  emptyCommand.command = L"";
  Check(PlanEnsure(emptyCommand, kExe, kVersion).decision == Decision::RefuseForeign,
        "ensure: a command that is empty is still a command someone wrote");
}

void TestTheOwnersDevelopmentKeyIsAdoptedByPolicy() {
  FakeRegistry registry;
  registry.key = DevMachine();
  Check(PlanEnsure(registry.key, kExe, kVersion).decision == Decision::RefuseForeign &&
            PlanEnsure(registry.key, kExe, kVersion).writes.empty(),
        "dev key: an unmarked key around another URnetwork.exe is refused by default");
  Policy policy;
  policy.adoptOtherUrnetworkExe = true;
  const Plan adopt = PlanEnsure(registry.key, kExe, kVersion, policy);
  Check(adopt.decision == Decision::Adopt && adopt.writes.size() == 4 && adopt.writes.front().name == kOwnerValue,
        "dev key: and adopted, marker first, when the policy says another URnetwork.exe is ours");
  registry.Apply(adopt);
  Check(registry.key.owner && registry.key.command == OpenCommand(kExe) &&
            PlanEnsure(registry.key, kExe, kVersion).decision == Decision::Current,
        "dev key: it then starts this exe and is current");
  Check(PlanRefresh(registry.key, kExe, kVersion).writes.empty() &&
            PlanRefresh(registry.key, LR"(D:\moved\URnetwork.exe)", kVersion).decision == Decision::Repoint,
        "dev key: and from then on it is ours to follow when the exe moves");
  UserKey lowerCase = DevMachine();
  lowerCase.command = LR"("d:\work\checkout\app\build\x64\release\urnetwork.exe" "%1")";
  Check(PlanEnsure(lowerCase, kExe, kVersion, policy).decision == Decision::Adopt,
        "dev key: the file name is compared without regard to case");
}

void TestAnUnmarkedKeyForThisExeIsAdopted() {
  UserKey same = DevMachine();
  same.command = OpenCommand(kExe);
  const Plan plan = PlanEnsure(same, kExe, kVersion);
  Check(plan.decision == Decision::Adopt && plan.writes.size() == 4 && plan.writes.front().name == kOwnerValue,
        "adopt: an unmarked key that already starts THIS exe is marked (all four values, marker first)");
  Check(PlanEnsure(same, kExe, kVersion, Policy{}).decision == Decision::Adopt, "adopt: with no policy at all");
  UserKey otherCase = same;
  otherCase.command = OpenCommand(LR"(C:\PROGRAM FILES\URNETWORK\URNETWORK.EXE)");
  Check(PlanEnsure(otherCase, kExe, kVersion).decision == Decision::Adopt, "adopt: the path is compared without regard to case");
  UserKey hollow;
  hollow.exists = true;
  Check(PlanEnsure(hollow, kExe, kVersion).decision == Decision::Adopt, "adopt: a key that names no handler is filled in");
  hollow.description = L"URL:Something Else";
  hollow.urlProtocol = true;
  Check(PlanEnsure(hollow, kExe, kVersion).decision == Decision::Adopt, "adopt: with or without a description");
  // an app update moved the exe: the marked key follows, but an unmarked one that names the old path does not
  UserKey oldPath = DevMachine();
  Check(PlanEnsure(oldPath, kExe, kVersion).decision == Decision::RefuseForeign,
        "adopt: an unmarked key for another path is not this exe's");
}

void TestUnsafePathsAreRefused() {
  const std::wstring tooLong = L"C:\\" + std::wstring(32000, L'a') + L".exe";
  for (const std::wstring_view bad :
       {std::wstring_view(L"URnetwork.exe"), std::wstring_view(LR"(\\server\share\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a"b\URnetwork.exe)"), std::wstring_view(LR"(C:\a%b\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a\URnetwork.dll)"), std::wstring_view(L""), std::wstring_view(LR"(C:\a\URnetwork.exe:stream)"), std::wstring_view(LR"(C:\dir:stream\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a\..\b\URnetwork.exe)"), std::wstring_view(LR"(C:\a\.\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a<b\URnetwork.exe)"), std::wstring_view(LR"(C:\a>b\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a|b\URnetwork.exe)"), std::wstring_view(LR"(C:\a*b\URnetwork.exe)"),
        std::wstring_view(LR"(C:\a?b\URnetwork.exe)"), std::wstring_view(L"C:\\a\tb\\URnetwork.exe"),
        std::wstring_view(L"C:\\a\nb\\URnetwork.exe"), std::wstring_view(LR"(C:URnetwork.exe)"),
        std::wstring_view(LR"(\URnetwork.exe)"), std::wstring_view(LR"(C:\.exe)"), std::wstring_view(LR"(1:\a\URnetwork.exe)"),
        std::wstring_view(tooLong)}) {
    Check(PlanEnsure(UserKey{}, bad, kVersion).decision == Decision::RefuseUnsafePath &&
              PlanEnsure(UserKey{}, bad, kVersion).writes.empty(),
          "ensure: an unsafe exe path is refused, with no writes");
    Check(PlanEnsure(Ours(), bad, kVersion).decision == Decision::RefuseUnsafePath, "ensure: also over our own key");
    Check(PlanRefresh(Ours(), bad, kVersion).decision == Decision::RefuseUnsafePath && PlanRefresh(Ours(), bad, kVersion).writes.empty(),
          "refresh: also at a launch, which then leaves the key as it is");
  }
  Check(SafeExePath(kExe) && SafeExePath(LR"(c:\a.exe)") && SafeExePath(LR"(D:\My (x86) Apps\URnetwork.exe)") &&
            SafeExePath(L"C:\\Users\\\u00fcn\u00ef\\URnetwork.exe") && SafeExePath(LR"(C:\Users\a b\URnetwork.EXE)"),
        "paths: ordinary ones are safe: spaces, parentheses, non-ASCII, a capital extension");
  Check(SafeExePath(LR"(C:\a.exe)") && SafeExePath(LR"(C:\ab.exe)"), "paths: a name of one letter will do");

  // the edges of each rule: the drive letters are A-Z and a-z, and the characters beside those
  // ranges are not drives
  for (const wchar_t letter : {L'A', L'Z', L'a', L'z'}) {
    Check(SafeExePath(std::wstring(1, letter) + LR"(:\a.exe)"), "paths: the drive letter code " + std::to_string(static_cast<int>(letter)) + " is a drive");
  }
  for (const wchar_t notLetter : {L'@', L'[', L'`', L'{', L'0', L'9', L' '}) {
    Check(!SafeExePath(std::wstring(1, notLetter) + LR"(:\a.exe)"), "paths: the character code " + std::to_string(static_cast<int>(notLetter)) + " is no drive");
  }
  // the first control character is refused, a space is not, and a file name is more than its extension
  Check(!SafeExePath(std::wstring(LR"(C:\a)") + wchar_t(0x1f) + LR"(b\URnetwork.exe)"), "paths: a control character (the last of them, 0x1f) is refused");
  Check(!SafeExePath(std::wstring(LR"(C:\a)") + wchar_t(0x01) + LR"(b\URnetwork.exe)"), "paths: and the first one");
  Check(SafeExePath(std::wstring(LR"(C:\a)") + wchar_t(0x20) + LR"(b\URnetwork.exe)"), "paths: a space is not a control character");
  Check(!SafeExePath(LR"(C:\a\.exe)") && SafeExePath(LR"(C:\a\b.exe)"), "paths: a file named just .exe is refused, one letter before it is not");
  // the length cap: 32000 characters are fine, one more is not
  const std::wstring atTheCap = L"C:\\" + std::wstring(32000 - 3 - 4, L'a') + L".exe";
  const std::wstring pastTheCap = L"C:\\" + std::wstring(32000 - 3 - 4 + 1, L'a') + L".exe";
  Check(atTheCap.size() == 32000 && SafeExePath(atTheCap), "paths: 32000 characters are the longest that is safe");
  Check(pastTheCap.size() == 32001 && !SafeExePath(pastTheCap), "paths: 32001 are not");
}

// Windows paths compare without regard to case, at every character and at both ends of the alphabet.
void TestPathsCompareWithoutCase() {
  Check(Lower(L'A') == L'a' && Lower(L'Z') == L'z' && Lower(L'M') == L'm' && Lower(L'a') == L'a' && Lower(L'z') == L'z',
        "case: the capitals A to Z are lowered, from both ends");
  Check(Lower(L'@') == L'@' && Lower(L'[') == L'[' && Lower(L'0') == L'0' && Lower(L'\\') == L'\\',
        "case: and the characters just beside them are not");
  Check(EqualsIgnoreCase(L"Zebra", L"zEBRA") && EqualsIgnoreCase(L"", L"") && EqualsIgnoreCase(L"C:\\A.EXE", L"c:\\a.exe"),
        "case: the same letters in another case are equal");
  Check(!EqualsIgnoreCase(L"ab", L"bb") && !EqualsIgnoreCase(L"ba", L"bb") && !EqualsIgnoreCase(L"ab", L"aba") &&
            !EqualsIgnoreCase(L"aba", L"ab") && !EqualsIgnoreCase(L"@", L"`"),
        "case: a different letter at the first place, the last, or a different length is not equal");
}

void TestARefreshOnlyFollowsOurOwnKey() {
  Check(PlanRefresh(UserKey{}, kExe, kVersion).writes.empty() && PlanRefresh(UserKey{}, kExe, kVersion).decision == Decision::Absent,
        "refresh: never creates a key");
  const Plan foreign = PlanRefresh(Foreign(), kExe, kVersion);
  Check(foreign.decision == Decision::RefuseForeign && foreign.writes.empty(), "refresh: never touches another program's key");
  const Plan unmarkedMine = PlanRefresh([] {
    UserKey k = DevMachine();
    k.command = OpenCommand(kExe);
    return k;
  }(), kExe, kVersion);
  Check(unmarkedMine.writes.empty() && unmarkedMine.decision == Decision::RefuseForeign,
        "refresh: nor an unmarked key, even one an attempt would adopt");
  Check(PlanRefresh(Ours(LR"(D:\old\URnetwork.exe)"), kExe, kVersion).decision == Decision::Repoint,
        "refresh: our key follows a moved exe");
  Check(PlanRefresh(Ours(kExe, L"0.1.0"), kExe, kVersion).decision == Decision::Repoint,
        "refresh: and an update");
  Check(PlanRefresh(Ours(), kExe, kVersion).writes.empty() && PlanRefresh(Ours(), kExe, kVersion).decision == Decision::Current,
        "refresh: our current key needs nothing");
}

// A command that Windows runs through a COM delegate is not one a command line can rewrite: the value
// beside it decides what runs, so the key is someone else's whoever marked it, and nothing is written.
void TestAKeyWithADelegateIsNeverRewritten() {
  UserKey delegated = Foreign();
  delegated.delegateExecute = true;
  Check(PlanEnsure(delegated, kExe, kVersion).decision == Decision::RefuseForeign && PlanEnsure(delegated, kExe, kVersion).writes.empty(),
        "delegate: another program's key with a DelegateExecute value is refused, with no writes");
  UserKey ours = Ours();
  ours.delegateExecute = true;
  Check(PlanEnsure(ours, kExe, kVersion).decision == Decision::RefuseForeign && PlanEnsure(ours, kExe, kVersion).writes.empty(),
        "delegate: a marked key that gained one is refused, and not written over");
  Check(PlanRefresh(ours, kExe, kVersion).decision == Decision::RefuseForeign && PlanRefresh(ours, kExe, kVersion).writes.empty(),
        "delegate: and a launch leaves it as it is");
  UserKey hollow;
  hollow.exists = true;
  hollow.delegateExecute = true;
  Check(PlanEnsure(hollow, kExe, kVersion).decision == Decision::RefuseForeign,
        "delegate: a key with no command of its own but a delegate is not a hollow key to fill in");
  UserKey sameCommand = DevMachine();
  sameCommand.command = OpenCommand(kExe);
  sameCommand.delegateExecute = true;
  Check(PlanEnsure(sameCommand, kExe, kVersion).decision == Decision::RefuseForeign,
        "delegate: nor an unmarked key whose command is ours");
  Policy policy;
  policy.adoptOtherUrnetworkExe = true;
  UserKey dev = DevMachine();
  dev.delegateExecute = true;
  Check(PlanEnsure(dev, kExe, kVersion, policy).decision == Decision::RefuseForeign,
        "delegate: nor the development key, whatever the policy says");
  // an absent key has no delegate to speak of
  Check(PlanEnsure(UserKey{}, kExe, kVersion).decision == Decision::Create, "delegate: an absent key is created as ever");
}

void TestNamesOfTheOwnersExe() {
  Check(NamesUrnetworkExe(OpenCommand(LR"(D:\x\URnetwork.exe)")), "names: the MSI's shape around a URnetwork.exe");
  Check(NamesUrnetworkExe(OpenCommand(LR"(d:\x\urnetwork.EXE)")), "names: whatever the case");
  Check(!NamesUrnetworkExe(OpenCommand(LR"(D:\x\URnetworkUpdate.exe)")), "names: not the update helper");
  Check(!NamesUrnetworkExe(OpenCommand(LR"(D:\x\urnetworkd.exe)")), "names: nor the service");
  Check(!NamesUrnetworkExe(OpenCommand(LR"(D:\x\MyURnetwork.exe)")), "names: nor a file that only ends the same");
  Check(!NamesUrnetworkExe(L"") && !NamesUrnetworkExe(L"\"") && !NamesUrnetworkExe(LR"("" "%1")"), "names: no command, no name");
  Check(!ExeOfCommand(LR"("C:\a" "b\URnetwork.exe" "%1")").has_value(), "names: a quote inside the exe is no exe");
  Check(!ExeOfCommand(LR"("C:\a\URnetwork.exe" "%2")").has_value() && !ExeOfCommand(LR"("C:\a\URnetwork.exe")").has_value(),
        "names: a command that does not end in the quoted %1 is not the shape");
  // the shortest command that holds an exe, and the longest one that holds none
  Check(ExeOfCommand(LR"("x" "%1")") == std::optional<std::wstring>(L"x"), "names: the shortest exe a command can hold comes back");
  Check(!ExeOfCommand(LR"("" "%1")").has_value() && !ExeOfCommand(LR"(" "%1")").has_value() && !ExeOfCommand(L"").has_value(),
        "names: a command with no exe in it gives none");
  // a key that was never read says nothing: no key, no marker, no command, no delegate
  Check(!UserKey{}.exists && !UserKey{}.urlProtocol && !UserKey{}.delegateExecute && !UserKey{}.description && !UserKey{}.command &&
            !UserKey{}.owner,
        "key: a key that was never read says nothing");
}

void TestTheTokensAreClosed() {
  std::set<std::string> seen;
  bool lowercase = true;
  for (const Decision d : {Decision::Absent, Decision::Current, Decision::Create, Decision::Repoint, Decision::Adopt,
                           Decision::RefuseForeign, Decision::RefuseUnsafePath}) {
    const std::string token(ToString(d));
    seen.insert(token);
    for (const char c : token) lowercase = lowercase && ((c >= 'a' && c <= 'z') || c == '-');
  }
  Check(seen.size() == 7, "tokens: one per decision, none repeated");
  Check(lowercase, "tokens: lowercase letters and hyphens only");
  Check(ToString(Decision::Create) == "created" && ToString(Decision::Repoint) == "repointed" &&
            ToString(Decision::Adopt) == "adopted" && ToString(Decision::Current) == "unchanged" &&
            ToString(Decision::RefuseForeign) == "refused-foreign" && ToString(Decision::RefuseUnsafePath) == "unsafe-path",
        "tokens: the words the log reads");
}

}  // namespace

int main() {
  TestTheCommandIsTheMsisShape();
  TestAnAbsentKeyIsCreatedMarkerFirst();
  TestOurOwnStaleKeyIsRewritten();
  TestSomeoneElsesKeyIsLeftAlone();
  TestTheOwnersDevelopmentKeyIsAdoptedByPolicy();
  TestAnUnmarkedKeyForThisExeIsAdopted();
  TestUnsafePathsAreRefused();
  TestPathsCompareWithoutCase();
  TestARefreshOnlyFollowsOurOwnKey();
  TestAKeyWithADelegateIsNeverRewritten();
  TestNamesOfTheOwnersExe();
  TestTheTokensAreClosed();
  std::cout << (gCases - gFailures) << "/" << gCases << " protocol registration checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
