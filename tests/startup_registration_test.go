// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// "Launch URnetwork on system startup" (Common/StartupRegistration.h) as a C++
// spec run against a fake registry, with negative controls that put each
// defect back into a copy of the header.

// Compile the startup registration spec (app/tools/startup-registration-tests.cpp)
// against Common/StartupRegistration.h and Common/InstanceHandover.h. `mutate`,
// when set, rewrites a copy of StartupRegistration.h for a negative control.
func startupRegistrationTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("startup registration tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"StartupRegistration.h", "InstanceHandover.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if mutate != nil && name == "StartupRegistration.h" {
			content = mutate(content)
			if content == string(source) {
				t.Fatal("negative control did not change the production StartupRegistration.h")
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "startup-registration-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "startup-registration-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build startup registration tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: off by default and nothing registered at a first run; on
// writes a Run value that starts this install with --autostart, so a sign-in
// shows only the tray icon; off deletes it and Task Manager's record of it; a
// registration Task Manager switched off shows as off and turning it on here
// clears that; and a launch brings an existing registration up to date
// without ever creating one.
func TestStartupRegistration(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="StartupRegistration.h" />`) {
		t.Error("Common.vcxproj does not list StartupRegistration.h")
	}
	program := startupRegistrationTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("startup registration: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireStartupRegistrationFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := startupRegistrationTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old replaced; a control whose text no
// longer matches fails rather than passing vacuously.
func startupRegistrationReplace(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("negative control expects exactly one %q in StartupRegistration.h", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// macOS's default is off. Turn it on by default, or let a launch create the
// registration, and the spec must fail.
func TestStartupRegistrationRejectsADefaultOtherThanMacOS(t *testing.T) {
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"inline constexpr bool kDefaultEnabled = false;", "inline constexpr bool kDefaultEnabled = true;")
	}, "default: off until the user turns it on")
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"  if (registration.command && *registration.command != command) {\n",
			"  if (registration.command != command) {\n")
	}, "first run: a launch registers nothing by itself")
}

// Turning it off must remove what turning it on wrote, and Task Manager's
// record of it. Leave either, and the spec must fail.
func TestStartupRegistrationRejectsAnOffThatLeavesARegistration(t *testing.T) {
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"    plan.deleteCommand = registration.command.has_value();\n", "")
	}, "off: the Run value is deleted")
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"    plan.deleteApproval = registration.approval.has_value();\n", "")
	}, "off: and Task Manager's record of it")
}

// Task Manager can switch the registration off. The toggle must show that,
// turning it on here must clear it, and a launch must leave it alone. Break
// any of those, and the spec must fail.
func TestStartupRegistrationRejectsIgnoringTaskManager(t *testing.T) {
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"  return registration.command.has_value() && !DisabledInTaskManager(registration);\n",
			"  return registration.command.has_value();\n")
	}, "task manager: a registration switched off there shows as off")
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"    plan.deleteApproval = DisabledInTaskManager(registration);\n", "")
	}, "task manager: turning it on here clears the 'disabled' Task Manager recorded")
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"    plan.writeCommand = std::wstring(command);\n  }\n  return plan;\n}\n\n}  // namespace urnw::startup",
			"    plan.writeCommand = std::wstring(command);\n  }\n  plan.deleteApproval = registration.approval.has_value();\n  return plan;\n}\n\n}  // namespace urnw::startup")
	}, "task manager: a launch never overrides the user's choice there")
}

// A launch brings an existing registration up to date: another path, or one
// without --autostart. Skip that, and the spec must fail.
func TestStartupRegistrationRejectsAStaleRegistration(t *testing.T) {
	requireStartupRegistrationFailure(t, func(source string) string {
		return startupRegistrationReplace(t, source,
			"  if (registration.command && *registration.command != command) {\n    plan.writeCommand = std::wstring(command);\n  }\n",
			"  (void)command;\n  (void)registration;\n")
	}, "refresh: a registration of another path now starts this install")
}

// The exe is quoted, so a path with spaces stays one argument and the
// argument after it is read as --autostart. Drop the quotes, and the spec
// must fail.
func TestStartupRegistrationRejectsAnUnquotedCommand(t *testing.T) {
	requireStartupRegistrationFailure(t, func(source string) string {
		unquoted := startupRegistrationReplace(t, source,
			"  std::wstring command = L\"\\\"\";\n", "  std::wstring command;\n")
		return startupRegistrationReplace(t, unquoted, "  command.append(L\"\\\" \");\n", "  command.append(L\" \");\n")
	}, "command: the exe is quoted")
}
