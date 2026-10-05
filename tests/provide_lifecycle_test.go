// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile one of the provide-while-disconnected harnesses against the shared
// headers (Common/ProvideLifecycle.h, and Common/Protocol.h for the wire
// spec). The headers are copied into an isolated include dir, so `mutate`,
// when set, can rewrite one of them for a negative control and the copy of the
// other picks the rewritten one up. `extra` adds compiler flags (the json
// include for the protocol spec).
func provideTestProgram(t *testing.T, harness string, mutate map[string]func(string) string,
	extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("provide tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	commonDir := filepath.Join(root, "app", "src", "Common")
	for _, name := range []string{"ProvideLifecycle.h", "Protocol.h"} {
		source, err := os.ReadFile(filepath.Join(commonDir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[name]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, strings.TrimSuffix(harness, ".cpp"))
	args := append([]string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir},
		extra...)
	args = append(args, filepath.Join(root, "app", "tools", harness), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build %s: %v\n%s", harness, err, output)
	}
	return program
}

// Execute the lifecycle spec: the sdk tier for every mode connected and
// disconnected, the service's refusals, and the app's step for every state.
func TestProvideLifecycle(t *testing.T) {
	program := provideTestProgram(t, "provide-lifecycle-tests.cpp", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide lifecycle: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run a mutated copy of the lifecycle and require that it fails, naming `want`.
func requireProvideFailure(t *testing.T, harness string, mutate map[string]func(string) string,
	want string, extra ...string) {
	t.Helper()
	program := provideTestProgram(t, harness, mutate, extra...)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// THE DEFECT: on Windows every mode provided only while connected. Put that
// rule back and Always, Auto and Network must fail while disconnected.
func TestProvideLifecycleRejectsProvidingOnlyWhileConnected(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return SdkTierFor(mode, connected) != Tier::None;",
				"return connected && SdkTierFor(mode, connected) != Tier::None;", 1)
		},
	}, "always provides while disconnected")
}

// Auto provides publicly only while connected (the sdk); idle, it serves the
// user's own devices. A table that made it public while idle must fail.
func TestProvideLifecycleRejectsAutoPublicWhileIdle(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return connected ? Tier::Public : Tier::Network;",
				"return connected ? Tier::Public : Tier::Public;", 1)
		},
	}, "tiers: auto disconnected")
}

// The service must not start a provider that would leave through the armed
// floor's own permit.
func TestProvideLifecycleRejectsProviderUnderArmedFloor(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  if (state.firewallInForce) return ProviderRefusal::KillSwitchArmed;\n", "", 1)
		},
	}, "refusal: the kill switch is armed after an unexpected drop")
}

// Nor beside a tunnel session: two devices under one identity.
func TestProvideLifecycleRejectsProviderBesideTunnel(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  if (state.tunnelSession) return ProviderRefusal::TunnelSession;\n", "", 1)
		},
	}, "refusal: a tunnel session exists or is starting")
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"if (!facts.answered || facts.tunnelSession) return DisconnectedStep::None;",
				"if (!facts.answered) return DisconnectedStep::None;", 1)
		},
	}, "step: a tunnel session exists")
}

// Never stops a provider the app finds running; a step that left it running
// must fail.
func TestProvideLifecycleRejectsNeverLeavingTheProvider(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return facts.providerRunning ? DisconnectedStep::Stop : DisconnectedStep::None;\n  }\n  // The armed floor",
				"return DisconnectedStep::None;\n  }\n  // The armed floor", 1)
		},
	}, "step: a running provider is stopped")
}

// The wire spec needs nlohmann/json, like the service and the app; a host
// without it skips (set URNETWORK_JSON_INCLUDE).
func provideJsonFlags(t *testing.T) []string {
	t.Helper()
	jsonDir, found := jsonIncludeDir(repositoryRoot(t))
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	if jsonDir == "" {
		return nil
	}
	return []string{"-isystem", jsonDir}
}

// Execute the wire spec: the verbs, the request, the status fields and an older
// service's silence, the device comparison and the facts the reconcile reads.
func TestProvideProtocol(t *testing.T) {
	extra := provideJsonFlags(t)
	program := provideTestProgram(t, "provide-protocol-tests.cpp", nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide protocol: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// A status reader that drops the provider bit shows "not providing" over a
// provider that is; a comparison that ignores the provider transport policy
// keeps a device running on the old one.
func TestProvideProtocolRejectsDroppedFields(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "  get(\"provider_running\", v.provider_running);\n", "", 1)
		},
	}, "status: provider_running round-trips", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				" &&\n         a.provider_transport_settings_json == b.provider_transport_settings_json;",
				";", 1)
		},
	}, "same device: a different provider_transport_settings_json builds a new device", extra...)
}
