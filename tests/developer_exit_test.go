// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The developer page's exit policy part (App/DeveloperExitPresentation.h): the
// built-in security rules generation of each exit's provider, in the exit's
// State cell. The C++ spec runs alone and against the generated sdk header,
// and the page's wiring is read from the app sources, which cannot be built
// off Windows.

// Compile the developer exit spec (app/tools/developer-exit-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildDeveloperExitTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("developer exit tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "developer-exit-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "developer-exit-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build developer exit tests: %v\n%s", err, output)
	}
	return program
}

// Run a built spec and log what it printed.
func runDeveloperExitTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("developer exit: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the developer exit spec: no policy part before the
// provider's first diagnostics, "unknown" for a provider without a
// generation, and the number otherwise.
func TestDeveloperExitPresentation(t *testing.T) {
	runDeveloperExitTests(t, buildDeveloperExitTests(t))
}

// The same spec built against the generated sdk header, so the template is
// instantiated with urnet::Exit and an exit parsed through the header's json
// carries the generation under the name the sdk writes. The header is
// git-ignored (fetch-deps unpacks it into app/third_party/urnetwork-sdk/<arch>;
// URNETWORK_SDK_INCLUDE names another directory) and needs nlohmann/json, so a
// host without them skips this, and so does a copy from before the sdk carried
// the generation.
func TestDeveloperExitPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := developerExitSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp whose Exit carries ProviderSecurityPolicyGeneration (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_DEVELOPER_EXIT_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runDeveloperExitTests(t, buildDeveloperExitTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp whose Exit carries the provider's
// security policy generation, or "".
func developerExitSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !strings.Contains(string(header), "int64_t ProviderSecurityPolicyGeneration{};") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp's Exit has no ProviderSecurityPolicyGeneration", explicit)
			}
			t.Logf("%s: urnetwork_sdk.hpp predates the exit's policy generation", dir)
			continue
		}
		return dir
	}
	return ""
}

// The page cannot be built off Windows, so its half is checked on its source:
// the exit table's State cell takes the policy part from the presentation for
// every exit, with the store's two strings, which exist in the neutral
// catalog with the generation's placeholder; the project lists the header.
func TestDeveloperExitPolicyGenerationWiring(t *testing.T) {
	page := readAppSource(t, "DeveloperPage.cpp")
	for _, want := range []string{
		`#include "DeveloperExitPresentation.h"`,
		`if (auto policy = developerexit::PolicyGenerationOf(e)) {`,
		`urnw::Format("dev_exit_policy_generation", *policy->generation)`,
		`urnw::Localized("dev_exit_policy_generation_unknown")`,
	} {
		if !strings.Contains(page, want) {
			t.Errorf("DeveloperPage.cpp is missing %q", want)
		}
	}
	// the part joins the State cell's parts after proven, before the cell is written
	proven := strings.Index(page, `if (e.Proven) state.push_back(DevW("dev_state_proven", L"proven"));`)
	policy := strings.Index(page, `developerexit::PolicyGenerationOf(e)`)
	written := strings.Index(page, `cells.state.Text(hstring{joined});`)
	if proven < 0 || policy < proven || written < policy {
		t.Error("the policy part is not composed into the State cell after proven")
	}

	root := repositoryRoot(t)
	for key, want := range map[string]string{
		"dev_exit_policy_generation":         "policy generation {}",
		"dev_exit_policy_generation_unknown": "policy generation unknown",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("%s = %q in the neutral catalog, want %q", key, got, want)
		}
	}
	if got := reswValue(t, root, "de", "dev_exit_policy_generation"); got != "Richtliniengeneration {}" {
		t.Errorf("dev_exit_policy_generation de = %q", got)
	}

	project := readAppSource(t, "App.vcxproj")
	if !strings.Contains(project, `<ClInclude Include="DeveloperExitPresentation.h" />`) {
		t.Error("App.vcxproj does not list DeveloperExitPresentation.h")
	}
}
