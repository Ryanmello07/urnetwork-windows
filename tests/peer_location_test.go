// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// The network peer location (App/PeerLocation.h) as a C++ spec, alone and
// against the generated sdk header.

// Compile the network peer location spec (app/tools/peer-location-tests.cpp).
// extra adds compiler arguments ahead of the source.
func buildPeerLocationTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("peer location tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	program := filepath.Join(t.TempDir(), "peer-location-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(appDir, "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(appDir, "tools", "peer-location-tests.cpp"),
		"-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build peer location tests: %v\n%s", err, output)
	}
	return program
}

// Run a built spec on the app directory and log what it printed.
func runPeerLocationTests(t *testing.T, program string) {
	t.Helper()
	appDir := filepath.Join(repositoryRoot(t), "app")
	if output, err := exec.Command(program, appDir).CombinedOutput(); err != nil {
		t.Fatalf("peer location: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the network peer location spec (App/PeerLocation.h): a
// tap on one of the user's own devices in the chooser sheet or on the Network
// page connects to the peer's client id with network_peer set (the Network
// provide mode, as on android and apple), named like the row; SdkHost's
// re-select no-op does not swallow the tap on a device that was picked as a
// public exit; and both rows and SdkHost::ConnectFromRow use the header.
func TestPeerLocation(t *testing.T) {
	runPeerLocationTests(t, buildPeerLocationTests(t))
}

// The same spec built against the generated SDK header, so the templates are
// instantiated with urnet::NetworkPeer and urnet::ConnectLocation and the
// location is checked as the json the SDK receives. Any header new enough for
// the VLESS API has ConnectLocation.network_peer, so this finds it the way
// TestVlessPresentationAgainstSdkHeader does, and skips the same way.
func TestPeerLocationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := vlessSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no current urnetwork_sdk.hpp (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_PEER_LOCATION_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runPeerLocationTests(t, buildPeerLocationTests(t, extra...))
}
