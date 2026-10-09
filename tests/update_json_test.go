// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The release list and last-result.json readers (Common/ReleaseJson.h and
// Common/UpdateResultJson.h), compiled and run by app/tools/update-json-tests.cpp.
// Skips without nlohmann/json, as the provide protocol harness does.

// The Common headers the program includes.
var updateJsonHeaders = []string{
	"ReleaseJson.h", "ReleaseSelection.h", "UpdateFormats.h", "UpdateResult.h", "UpdateResultJson.h",
	"VersionGrammar.h",
}

func updateJsonTestProgram(t *testing.T, header string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("update json tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range updateJsonHeaders {
		data, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", name))
		if err != nil {
			t.Fatal(err)
		}
		source := string(data)
		if name == header {
			changed := mutate(source)
			if changed == source {
				t.Fatalf("negative control did not change %s", header)
			}
			source = changed
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(source), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "update-json-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "update-json-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build update-json-tests.cpp: %v\n%s", err, output)
	}
	return program
}

func TestUpdateJson(t *testing.T) {
	program := updateJsonTestProgram(t, "", nil)
	output, err := exec.Command(program).CombinedOutput()
	if err != nil {
		t.Fatalf("update json: %v\n%s", err, output)
	}
	t.Logf("%s", output)
}

// A reader that trusts a field's type, or skips a check of the report, fails
// a named check.
func TestUpdateJsonRejectsLooserReaders(t *testing.T) {
	for _, tc := range []struct {
		name, header, old, replacement, want string
	}{
		{"immutable from any value", "ReleaseJson.h",
			`release.immutable = json_detail::Flag(item, "immutable");`,
			`release.immutable = item.contains("immutable");`,
			"fields of the wrong type read as empty and false"},
		{"immutable never read", "ReleaseJson.h",
			`release.immutable = json_detail::Flag(item, "immutable");`, "",
			"a published immutable release reads as one"},
		{"the publication time never read", "ReleaseJson.h",
			`release.publishedAt = json_detail::Text(item, "published_at");`, "",
			"its publication time is published_at"},
		{"the draft's creation time taken for the publication time", "ReleaseJson.h",
			`release.publishedAt = json_detail::Text(item, "published_at");`,
			`release.publishedAt = json_detail::Text(item, "created_at");`,
			"its publication time is published_at"},
		{"a publication time of any type", "ReleaseJson.h",
			`release.publishedAt = json_detail::Text(item, "published_at");`,
			`release.publishedAt = item.contains("published_at") ? item["published_at"].dump() : std::string{};`,
			"a null published_at reads as none"},
		{"report code from a string", "UpdateResultJson.h",
			"!code->is_number_unsigned() ||", "", `not a report: {"tag":"v2026.10.1-1060587890","code":"1060587890"`},
		{"report not checked", "UpdateResultJson.h",
			"if (!IsWellFormed(result, tagPrefix)) return std::nullopt;", "(void)tagPrefix;",
			"not a report: {\"tag\":\"latest\""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			program := updateJsonTestProgram(t, tc.header, func(source string) string {
				if strings.Count(source, tc.old) != 1 {
					return source
				}
				return strings.Replace(source, tc.old, tc.replacement, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), tc.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", tc.want, err, output)
			}
		})
	}
}
