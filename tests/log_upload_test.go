// SPDX-License-Identifier: MPL-2.0

// The log upload spec (app/tools/log-upload-tests.cpp) built against the
// shared headers and run, with its negative controls: a mutated copy of a
// header must fail the harness, naming the check that guards that defect.

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Builds the log upload harness against copies of the shared headers
// (Common/LogUpload.h, and Common/Protocol.h with the ProvideLifecycle.h it
// includes) in an isolated include directory, so `mutate`, when set, can
// rewrite one of them for a negative control. Skips without nlohmann/json, as
// the provide protocol harness does.
func logUploadTestProgram(t *testing.T, mutate map[string]func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("log upload tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	commonDir := filepath.Join(root, "app", "src", "Common")
	for _, headerName := range []string{"LogUpload.h", "Protocol.h", "ProvideLifecycle.h"} {
		source, err := os.ReadFile(filepath.Join(commonDir, headerName))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[headerName]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", headerName)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, headerName), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "log-upload-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "log-upload-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build log-upload-tests.cpp: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the carrier, the standalone device's refusals and
// lifetime, the app's fallback, the request and reply on the wire and the
// feedback id check.
func TestLogUpload(t *testing.T) {
	program := logUploadTestProgram(t, nil)
	output, err := exec.Command(program).CombinedOutput()
	if err != nil {
		t.Fatalf("log upload: %v\n%s", err, output)
	}
	t.Logf("%s", output)
}

// Put each defect back in a copy of its header and require the harness to
// fail on the check that names it.
func TestLogUploadNegativeControls(t *testing.T) {
	type negativeControl struct {
		defect     string
		headerName string
		find       string
		replace    string
		want       string
	}
	controls := []negativeControl{
		{
			defect:     "the provider-only device carries it while a session runs",
			headerName: "LogUpload.h",
			find:       "  if (sessionDevice) return Carrier::Tunnel;\n  if (providerDevice) return Carrier::Provider;",
			replace:    "  if (providerDevice) return Carrier::Provider;\n  if (sessionDevice) return Carrier::Tunnel;",
			want:       "carrier: the session's device wins should both ever run",
		},
		{
			defect:     "a standalone device beside a running provider-only device",
			headerName: "LogUpload.h",
			find:       "  if (providerDevice) return Carrier::Provider;\n",
			replace:    "  (void)providerDevice;\n",
			want:       "carrier: the provider-only device while there is no session",
		},
		{
			defect:     "a standalone device while a teardown still holds the identity",
			headerName: "LogUpload.h",
			find:       "  if (deviceStillHeld) return StandaloneRefusal::DeviceStillHeld;\n",
			replace:    "  (void)deviceStillHeld;\n",
			want:       "refusal: a teardown still holds this identity",
		},
		{
			defect:     "a standalone device that outlives a stuck upload by hours",
			headerName: "LogUpload.h",
			find:       "kStandaloneDeviceMaxLifetime{30}",
			replace:    "kStandaloneDeviceMaxLifetime{300}",
			want:       "lifetime: 30 minutes without a report",
		},
		{
			defect:     "the app uploads through the DeviceRemote beside the service",
			headerName: "LogUpload.h",
			find:       "  if (serviceAccepted) return AppStep::Done;\n  if (deviceRemoteBound) return AppStep::DeviceRemote;",
			replace:    "  if (deviceRemoteBound) return AppStep::DeviceRemote;\n  if (serviceAccepted) return AppStep::Done;",
			want:       "app: no second upload through the DeviceRemote beside the service's",
		},
		{
			defect:     "a path separator in the feedback id",
			headerName: "Protocol.h",
			find:       "      if (c != '-') return false;\n",
			replace:    "",
			want:       `feedback id: refuses "f00dfeed/0000-4000-8000-000000000001"`,
		},
		{
			defect:     "the feedback id left off the wire",
			headerName: "Protocol.h",
			find:       "      {\"feedback_id\", v.feedback_id},\n",
			replace:    "",
			want:       "wire: feedback_id",
		},
		{
			defect:     "the carrier left off the reply",
			headerName: "Protocol.h",
			find:       "  if (!v.log_upload_carrier.empty()) j[\"log_upload_carrier\"] = v.log_upload_carrier;\n",
			replace:    "",
			want:       "reply: the carrier comes back",
		},
	}
	for _, control := range controls {
		find, replace := control.find, control.replace
		program := logUploadTestProgram(t, map[string]func(string) string{
			control.headerName: func(source string) string {
				return strings.Replace(source, find, replace, 1)
			},
		})
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s", control.defect,
				control.want, err, output)
		}
	}
}
