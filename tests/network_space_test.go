// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Ids.h needs a GUID for the tray icon and tun adapter ids; on a non-Windows
// host the tests supply the one typedef <guiddef.h> would.
const guiddefStub = `#pragma once
typedef struct _GUID {
  unsigned long Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char Data4[8];
} GUID;
`

// Compile and execute the bundled network space spec (Common/Ids.h,
// Common/NetworkSpaceStartup.h): the operator host, the legacy key re-keyed to
// it, the launch order, and the values the whole-values writers put over what
// a space stores. Both headers are copied into an isolated include dir so
// mutateIds, when set, can rewrite Ids.h for a negative control and the copied
// NetworkSpaceStartup.h picks the rewritten one up. extra adds compiler
// arguments ahead of the source.
func networkSpaceTestProgram(t *testing.T, mutateIds func(string) string, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("network space tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	commonDir := filepath.Join(root, "app", "src", "Common")
	for _, name := range []string{"Ids.h", "NetworkSpaceStartup.h"} {
		source, err := os.ReadFile(filepath.Join(commonDir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if name == "Ids.h" && mutateIds != nil {
			content = mutateIds(content)
			if content == string(source) {
				t.Fatal("negative control did not change the production ids")
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "guiddef.h"), []byte(guiddefStub), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "network-space-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "network-space-tests.cpp"), "-o", program)
	build := exec.Command(compiler, arguments...)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build network space tests: %v\n%s", err, output)
	}
	return program
}

func TestNetworkSpaceStartup(t *testing.T) {
	program := networkSpaceTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("network space startup: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The operator stays bringyour.com: the move to *.ur.network was cancelled, and
// a build that bundles the official space under ur.network again derives
// api.ur.network, a dead host. That has to fail the suite, not just a review.
func TestNetworkSpaceRejectsLegacyHost(t *testing.T) {
	program := networkSpaceTestProgram(t, func(source string) string {
		return strings.Replace(source, `kNetworkSpaceHostName[] = "bringyour.com"`,
			`kNetworkSpaceHostName[] = "ur.network"`, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "legacy ur.network key") {
		t.Fatalf("ur.network bundled host was not detected: %v\n%s", err, output)
	}
}

// The same spec built against the generated SDK header: the value writers
// instantiated with urnet::NetworkSpaceValues, and a stored export carried
// through them by the header's own json conversions, every value a user saves
// in a space kept. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and so
// does a copy from before the bootstrap DoH servers the export carries.
func TestNetworkSpaceValuesAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "control_doh_urls_ipv4")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the bootstrap DoH servers (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_NETWORK_SPACE_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	program := networkSpaceTestProgram(t, nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("network space values against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The directory of a urnetwork_sdk.hpp that names `marker`, or "".
func sdkHeaderDirWith(t *testing.T, root string, marker string) string {
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
		if !strings.Contains(string(header), marker) {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no %s", explicit, marker)
			}
			t.Logf("%s: urnetwork_sdk.hpp has no %s", dir, marker)
			continue
		}
		return dir
	}
	return ""
}

// SdkHost's two whole-values writers cannot be built off Windows, so what they
// must keep doing is checked on their source: each writes the values it owns
// over what the space stores under the key it writes (the space's own json,
// read by StoredSpaceValuesLocked), never a fresh set, which is what dropped the
// space's extender settings, private extender and bootstrap DoH servers on
// every launch.
func TestNetworkSpaceWritersStartFromTheStoredValues(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for name, writer := range map[string]string{
		"urnet::NetworkSpace SdkHost::BuildNetworkSpace(": "netspace::BundledSpaceValuesOver(",
		"bool SdkHost::ApplyNetworkServer(":               "netspace::ServerSpaceValuesOver(",
	} {
		body := functionBody(host, name)
		over := strings.Index(body, writer)
		stored := strings.Index(body, "StoredSpaceValuesLocked(key).value_or(urnet::NetworkSpaceValues{})")
		written := strings.Index(body, "updateNetworkSpaceValues(key, values)")
		if over < 0 || stored < over || written < 0 || written < stored {
			t.Errorf("%s does not write %s over the space's stored values before updateNetworkSpaceValues", name, writer)
		}
		if strings.Contains(body, "urnet::NetworkSpaceValues values;") || strings.Contains(body, "values.link_host_name =") {
			t.Errorf("%s builds the space's values from nothing; they replace the stored set whole", name)
		}
	}
}
