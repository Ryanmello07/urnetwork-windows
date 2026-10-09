// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// How a launch reaches the one running instance, how that instance takes it
// while it starts, runs and exits, and what the launch shows
// (Common/InstanceHandover.h), as a C++ spec, with negative controls that put
// each defect back into a copy of the header.

// Compile the instance hand-over spec (app/tools/instance-handover-tests.cpp)
// against Common/InstanceHandover.h. `mutate`, when set, rewrites a copy of the
// header for a negative control.
func instanceHandoverTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("instance hand-over tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Common")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "InstanceHandover.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production InstanceHandover.h")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "InstanceHandover.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "instance-handover-tests")
	build := exec.Command(compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "instance-handover-tests.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build instance hand-over tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: a running instance serves a launch through its UI thread;
// one that is starting holds it until the UI opens; one that is exiting
// refuses it, with its exiting signal raised first; and a launch refused by an
// exiting instance, or whose instance ended, starts the app once that instance
// has gone, while only an instance that still runs and is not exiting earns an
// "already running" message. The user's launch opens the window and an
// autostart shows only the tray icon, wherever each lands; and no launch
// starts the app while an update installs.
func TestInstanceHandover(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="InstanceHandover.h" />`) {
		t.Error("Common.vcxproj does not list InstanceHandover.h")
	}
	program := instanceHandoverTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("instance hand-over: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireInstanceHandoverFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := instanceHandoverTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func handoverReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in InstanceHandover.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect, on the launch's side: a redirect the quitting instance took
// ended the launch as handed over, and the instance never acted on it. Put
// that back (a taken redirect is a hand-over whatever the holder is doing)
// and the spec must fail.
func TestInstanceHandoverRejectsALaunchLostToAQuittingInstance(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"  if (attempt.holderExiting) return Step::AwaitHolderExitThenRegister;\n", "")
	}, "quit: a launch refused by a quitting instance starts the app once that instance has ended")
}

// The defect, on the instance's side: the launch was queued for a UI thread
// that was quitting and would never run it. Put back an exit that leaves a
// queued launch undecided, and the spec must fail.
func TestInstanceHandoverRejectsAnExitThatLeavesAQueuedLaunch(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"        if (it->second.outcome == Outcome::Undecided) it->second.outcome = Outcome::Refused;\n", "")
	}, "quit: a launch queued for a UI thread that is quitting is refused")
}

// A refused launch must never be acted on, and the refusal must come after
// the exiting signal, or a launch could read the refusal, find no signal and
// end as handed over.
func TestInstanceHandoverRejectsARefusalThatIsNotFinal(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"      const bool refused = it->second.outcome != Outcome::Undecided;\n",
			"      const bool refused = false;\n")
	}, "busy ui: a queued launch the exit overtakes is never acted on")
	requireInstanceHandoverFailure(t, func(source string) string {
		raise := "    if (raiseExitingSignal_) raiseExitingSignal_();\n"
		moved := handoverReplace(t, source, raise, "")
		return handoverReplace(t, moved,
			"        it = it->second.abandoned && !it->second.posted ? launches_.erase(it) : std::next(it);\n"+
				"      }\n    }\n",
			"        it = it->second.abandoned && !it->second.posted ? launches_.erase(it) : std::next(it);\n"+
				"      }\n    }\n"+raise)
	}, "quit: the exiting signal is raised before anything is refused")
}

// A redirect into an instance whose process ends first waits on an event
// nobody will set (the App SDK waits on nothing else). It used to time out
// after 15 s and report a running app that did not respond. Treat the ended
// holder as a failure again, and the spec must fail.
func TestInstanceHandoverRejectsWaitingOutAHolderThatEnded(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"  if (attempt.result == RedirectResult::HolderGone) return Step::RegisterAgain;\n", "")
	}, "gone: with nothing to wait for")
}

// The launch must not register, and start, while the exiting instance still
// holds the service's single-instance pipe; and an exiting instance that
// never ends is still closing, not running. Skip the wait, and the spec must
// fail.
func TestInstanceHandoverRejectsStartingBeforeTheHolderEnded(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"        if (!launcher.AwaitHolderExit()) return LaunchResult::StillClosing;\n", "")
	}, "quit: the launch waits for the exiting holder's process before it registers again")
}

// The redirect is held open until the instance has decided, which is what
// lets the launch tell a served launch from a refused one. Return at once
// after queueing the launch, as the old handler did, and the spec must fail.
func TestInstanceHandoverRejectsATakeThatDoesNotWaitForTheDecision(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "    return Await(id, deadline);\n",
			"    (void)deadline;\n    return Outcome::Undecided;\n")
	}, "running: Take returns once the UI thread has taken a plain launch (served)")
}

// Launches that reach an instance before its UI is up are served when the UI
// opens, and one the UI had not reached by the deadline is still served. Drop
// either, and the spec must fail.
func TestInstanceHandoverRejectsDroppingALaunch(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"        heldRequests.push_back(it->second.request);\n        it->second.outcome = Outcome::Served;\n", "")
	}, "starting: a launch held while the instance started is served when the UI opens")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "      it->second.abandoned = true;\n", "      launches_.erase(it);\n")
	}, "busy ui: a launch the UI had not reached by the deadline is still served")
}

// A UI thread that takes no more work means the instance is ending: the
// launch is refused with the signal raised, not left waiting on a queue that
// will never run. Remove that, and the spec must fail.
func TestInstanceHandoverRejectsWaitingOnAQueueThatTakesNoWork(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"      // The UI thread takes no more work, so this instance is ending.\n      Close();\n",
			"      // The UI thread takes no more work, so this instance is ending.\n")
	}, "no ui: a launch the UI thread will not take is refused")
}

// A launch makes a bounded number of rounds. Remove the bound, and the spec
// must fail.
func TestInstanceHandoverRejectsAnUnboundedLaunch(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"    if (round == kLaunchRounds) return LaunchResult::StillClosing;\n",
			"    if (round == kLaunchRounds * 1000) return LaunchResult::StillClosing;\n")
	}, "rounds: a launch gives up after kLaunchRounds rounds")
}

// The owner's 2026-10-05 decision: every launch the user starts opens the
// window, and an autostart at sign-in shows only the tray icon. Put back the
// old first launch, which went to the tray whoever started it, or let an
// autostart open the window, and the spec must fail.
func TestInstanceHandoverRejectsTheWrongWindowForALaunch(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"  return request.autostart ? LaunchAction::TrayOnly : LaunchAction::ShowWindow;\n",
			"  return LaunchAction::TrayOnly;\n")
	}, "user launch: the user's launch opens the window")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"  return request.autostart ? LaunchAction::TrayOnly : LaunchAction::ShowWindow;\n",
			"  return LaunchAction::ShowWindow;\n")
	}, "autostart: an autostart shows only the tray icon")
}

// The autostart argument is matched whole: a longer argument, or one inside a
// path or a deep link, is not an autostart. Match it as a prefix, and the spec
// must fail.
func TestInstanceHandoverRejectsAnAutostartArgumentMatchedLoosely(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		loose := handoverReplace(t, source,
			"      if (inToken && token == argument) return true;\n",
			"      if (inToken && token.starts_with(argument)) return true;\n")
		return handoverReplace(t, loose,
			"  return inToken && token == argument;\n",
			"  return inToken && token.starts_with(argument);\n")
	}, "arguments: --autostart is matched whole and unquoted")
}

// The relaunch after an update waits for the update that started it, for as
// long as it runs and at most the budget. Make it look once, give up at once,
// or wait without a bound or a poll short, or shorten the budget, and the spec
// must fail.
func TestInstanceHandoverRejectsARelaunchThatDoesNotWaitOutTheUpdate(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "  while (updating()) {\n", "  if (updating()) {\n")
	}, "after update: the relaunch waits while the update runs, and goes when it ends")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "    if (now() >= deadline) return false;\n", "    if (true) return false;\n")
	}, "after update: the relaunch waits while the update runs, and goes when it ends")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "    if (now() >= deadline) return false;\n",
			"    if (now() >= deadline + std::chrono::hours(24 * 365)) return false;\n")
	}, "after update: an update that never ends releases the relaunch at the budget")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "const std::chrono::milliseconds deadline = now() + budget;",
			"const std::chrono::milliseconds deadline = now() + budget - kAfterUpdatePoll;")
	}, "after update: the relaunch waits the whole budget, not a poll less")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source, "kAfterUpdateBudget{120000};", "kAfterUpdateBudget{10000};")
	}, "after update: the budget is two minutes, polled every half second")
}

// The owner's 2026-10-05 decision: no launch starts the app while an update
// installs. Drop the check every round makes, or the one before waiting out an
// instance that quits for the installer, and the spec must fail.
func TestInstanceHandoverRejectsALaunchThatStartsDuringAnUpdate(t *testing.T) {
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"    if (launcher.UpdateInProgress()) return LaunchResult::Updating;\n    if (launcher.OwnsKey()) return LaunchResult::Holder;\n",
			"    if (launcher.OwnsKey()) return LaunchResult::Holder;\n")
	}, "update: a launch during an update does not start the app")
	requireInstanceHandoverFailure(t, func(source string) string {
		return handoverReplace(t, source,
			"        if (launcher.UpdateInProgress()) return LaunchResult::Updating;\n        if (!launcher.AwaitHolderExit()) return LaunchResult::StillClosing;\n",
			"        if (!launcher.AwaitHolderExit()) return LaunchResult::StillClosing;\n")
	}, "update: a launch that meets the instance quitting for the installer exits without waiting")
}
