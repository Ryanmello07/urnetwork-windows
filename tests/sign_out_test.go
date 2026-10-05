// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Signing out of URnetwork (Common/SignOut.h) as a C++ spec run against a fake
// service, with negative controls that rewrite a copy of the header.

// Compile the sign-out spec (app/tools/sign-out-tests.cpp) against copies of
// Common/SignOut.h and Common/ProvideLifecycle.h. `mutate`, when set, rewrites
// the copy of SignOut.h for a negative control.
func signOutTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sign-out tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"SignOut.h", "ProvideLifecycle.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if mutate != nil && name == "SignOut.h" {
			content = mutate(content)
			if content == string(source) {
				t.Fatal("negative control did not change the production SignOut.h")
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "sign-out-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "sign-out-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build sign-out tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: a sign-out sends Quit's stop_tunnel and stop_provider, in
// Quit's order, then the logout; a service that missed it is told before
// anything else once it can be reached, after a restart, a relaunch or a
// reboot, and nothing of the old account runs or starts again; and the next
// sign-in starts what its settings say, as a fresh launch does.
func TestSignOut(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="SignOut.h" />`) {
		t.Error("Common.vcxproj does not list SignOut.h")
	}
	program := signOutTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sign-out: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireSignOutFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := signOutTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old rewritten; the control fails when
// the header no longer has exactly one.
func replaceSignOutOnce(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("SignOut.h no longer has exactly one %q; update this control", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The order is Quit's: the machine first, then the provider, then the
// identity.
func TestSignOutRejectsAnotherOrder(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		return replaceSignOutOnce(t, source,
			"{Request::StopTunnel, Request::StopProvider,\n                                                  Request::Logout}",
			"{Request::StopProvider, Request::StopTunnel,\n                                                  Request::Logout}")
	}, "Quit's requests in Quit's order")
}

// A sign-out that leaves out the logout leaves the account's identity and
// credential in the service.
func TestSignOutRejectsLeavingTheIdentity(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		source = replaceSignOutOnce(t, source, "std::array<Request, 3> kRequests", "std::array<Request, 2> kRequests")
		return replaceSignOutOnce(t, source, "Request::StopProvider,\n                                                  Request::Logout}",
			"Request::StopProvider}")
	}, "the service keeps neither the identity nor a's credential")
}

// The defect a missed sign-out had: nothing outlived the app, so a relaunch
// never told the service. Begin without the marker must fail.
func TestSignOutRejectsAnObligationThatDiesWithTheApp(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		return replaceSignOutOnce(t, source, "    owed_.store(true);\n    marker_.write(true);\n",
			"    owed_.store(true);\n")
	}, "relaunch: the owed sign-out survives the app")
}

// A refused request must keep the sign-out owed, or the next account starts
// beside the old one's provider and on its identity.
func TestSignOutRejectsClearingARefusal(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		return replaceSignOutOnce(t, source, "    if (!done) return Delivery::Refused;\n", "    (void)done;\n")
	}, "wedged: the sign-out stays owed")
}

// Every request goes out even when one fails: each does a part the others do
// not.
func TestSignOutRejectsStoppingAtTheFirstFailure(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		return replaceSignOutOnce(t, source, "if (!service.send(request)) done = false;",
			"if (!service.send(request)) return Delivery::Refused;")
	}, "one fails: stop_provider and logout still go out")
}

// With nothing owed a pass sends nothing: a logout there would sever a
// signed-in account's identity.
func TestSignOutRejectsDeliveringWhatIsNotOwed(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		return replaceSignOutOnce(t, source, "    if (!owed_.load()) return Delivery::Delivered;\n", "")
	}, "nothing owed: a pass sends no sign-out request")
}

// The marker is written before the first request: an app ended during the
// delivery still owes it.
func TestSignOutRejectsMarkingAfterTheRequests(t *testing.T) {
	requireSignOutFailure(t, func(source string) string {
		source = replaceSignOutOnce(t, source, "    owed_.store(true);\n    marker_.write(true);\n    return Settle(service);\n",
			"    owed_.store(true);\n    const Delivery delivery = Settle(service);\n    if (owed_.load()) marker_.write(true);\n    return delivery;\n")
		return source
	}, "marker: written before the first request")
}
