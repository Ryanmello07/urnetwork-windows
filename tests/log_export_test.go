// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Export logs (App/LogExport.h) as a C++ spec that writes real files in a scratch directory: the copy is the log with every
// urnetwork: link in it reduced, and nothing about it can empty, change or lose the log itself or an export that was
// already there. The picker suggests the log's own name, so saving over the log has to be refused before anything is
// opened for writing. Each negative control puts one defect back into a copy of the header. How the settings page calls it is
// auth_link_wiring_test.go (TestExportedLogsHaveNoLinks).

// Compile the spec (app/tools/log-export-tests.cpp) against App/LogExport.h and App/AuthLink.h. `mutate`, when
// set, rewrites a copy of LogExport.h for a negative control.
func logExportTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("log export tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"AuthLink.h", "LogExport.h"} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		content := lf(string(source))
		if mutate != nil && name == "LogExport.h" {
			changed := mutate(content)
			if changed == content {
				t.Fatal("negative control did not change the production LogExport.h")
			}
			content = changed
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "log-export-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "log-export-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build log export tests: %v\n%s", err, output)
	}
	return program
}

// Run the program in a scratch directory of its own.
func runLogExport(t *testing.T, program string) ([]byte, error) {
	t.Helper()
	return exec.Command(program, t.TempDir()).CombinedOutput()
}

func TestLogExport(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="LogExport.h" />`) {
		t.Error("App.vcxproj does not list LogExport.h")
	}
	program := logExportTestProgram(t, nil)
	if output, err := runLogExport(t, program); err != nil {
		t.Fatalf("log export: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The header is standard C++ and the parser's header, so that the spec runs on any host; and it opens the log for
// reading only: nothing in it writes to, renames or removes the source.
func TestLogExportHeaderIsPureAndOnlyReadsTheLog(t *testing.T) {
	source := lf(readAppSource(t, "LogExport.h"))
	allowed := map[string]bool{
		"<filesystem>": true, "<fstream>": true, "<string>": true, "<system_error>": true, `"AuthLink.h"`: true,
	}
	for _, include := range regexp.MustCompile(`(?m)^[ \t]*#[ \t]*include[ \t]*([<"][^>"]+[>"])`).FindAllStringSubmatch(source, -1) {
		if !allowed[include[1]] {
			t.Errorf("LogExport.h includes %s: it is standard C++ and the parser, so it runs on any host", include[1])
		}
	}
	code := stripComments(source)
	quitForbid(t, "LogExport.h", code, "the log itself is read and never written, renamed, resized or removed",
		"std::ofstream out(source", "std::filesystem::remove(", "resize_file(", "rename(", "copy_file(", "winrt::", "windows.h", "std::ios::app", "std::fstream")
	// the destination is opened for writing once, after the source has been found to be a file, opened and found not to be it
	copyBody := definitionBody(t, "LogExport.h", code, "inline bool CopyWithoutLinks(const std::filesystem::path& source, const std::filesystem::path& destination) {")
	signOutRequireInOrder(t, "CopyWithoutLinks", copyBody,
		regexp.QuoteMeta("if (!std::filesystem::is_regular_file(source, unknown)) return false;"),
		regexp.QuoteMeta("std::ifstream in(source, std::ios::binary);"),
		regexp.QuoteMeta("if (!in) return false;"),
		regexp.QuoteMeta("std::filesystem::equivalent(source, destination, unknown)"),
		regexp.QuoteMeta("std::ofstream out(destination, std::ios::binary | std::ios::trunc);"),
		regexp.QuoteMeta("while (std::getline(in, line)) {"),
		regexp.QuoteMeta("out << authlink::RedactLogText(line);"))
	if strings.Count(copyBody, "std::ofstream") != 1 {
		t.Error("CopyWithoutLinks opens a file for writing more than once")
	}
}

// Each defect below puts one thing back that an export has gone wrong with. The spec must fail naming the check. A
// defect that only a host's C++ library can show names the line the spec prints on such a host, and is skipped, saying
// what the spec said instead, on any other.
func TestLogExportSpecDetectsEachDefectPutBack(t *testing.T) {
	cases := []struct {
		name string
		old  string
		new  string
		want string
		host string
	}{
		{
			name: "saving over the log is allowed, and empties it",
			old:  "  if (std::filesystem::equivalent(source, destination, unknown)) return false;\n",
			new:  "  (void)unknown;\n",
			want: "same file: saving over the log is refused",
		},
		{
			name: "the destination is opened before the source",
			old:  "  std::error_code unknown;\n  if (!std::filesystem::is_regular_file(source, unknown)) return false;\n  std::ifstream in(source, std::ios::binary);\n  if (!in) return false;\n  if (std::filesystem::equivalent(source, destination, unknown)) return false;\n  std::ofstream out(destination, std::ios::binary | std::ios::trunc);\n  if (!out) return false;\n",
			new:  "  std::ofstream out(destination, std::ios::binary | std::ios::trunc);\n  std::error_code unknown;\n  if (!std::filesystem::is_regular_file(source, unknown)) return false;\n  std::ifstream in(source, std::ios::binary);\n  if (!in || !out) return false;\n  if (std::filesystem::equivalent(source, destination, unknown)) return false;\n",
			want: "an export that was already there is as it was",
		},
		{
			name: "the links are copied as they are",
			old:  "    out << authlink::RedactLogText(line);\n",
			new:  "    out << line;\n",
			want: "no value survives",
		},
		{
			name: "a last line gets a line break it did not have",
			old:  "    if (!in.eof()) out << '\\n';  // the last line of a log may end without one\n",
			new:  "    out << '\\n';\n",
			want: "a last line that ends without a line break is not given one",
		},
		{
			name: "a line break is lost",
			old:  "    if (!in.eof()) out << '\\n';  // the last line of a log may end without one\n",
			new:  "",
			want: "every link of every line is its host and a length",
		},
		{
			name: "an earlier export is appended to",
			old:  "std::ofstream out(destination, std::ios::binary | std::ios::trunc);",
			new:  "std::ofstream out(destination, std::ios::binary | std::ios::app);",
			want: "an earlier export is replaced whole",
		},
		{
			// a source that merely exists is taken for a log: a directory exists, whether this host opens it or not
			name: "a directory is copied as a log",
			old:  "  if (!std::filesystem::is_regular_file(source, unknown)) return false;\n  std::ifstream in(source, std::ios::binary);\n  if (!in) return false;\n",
			new:  "  if (!std::filesystem::exists(source, unknown)) return false;\n  std::ifstream in(source, std::ios::binary);\n",
			want: "directory: a directory is not copied as a log, and nothing is made of the copy",
		},
		{
			// a source taken for a file because it opened, as before the kind was asked: Linux opens a directory for
			// reading, and the copy has been made by the time the read fails. Windows does not open one, so there the
			// open refuses it and this cannot show
			name: "a source that opens is taken for a file",
			old:  "  if (!std::filesystem::is_regular_file(source, unknown)) return false;\n",
			new:  "",
			want: "directory: a directory is not copied as a log, and nothing is made of the copy",
			host: "host: a directory opens for reading here",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			program := logExportTestProgram(t, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in LogExport.h", c.old)
				}
				return strings.Replace(source, c.old, c.new, 1)
			})
			output, err := runLogExport(t, program)
			if c.host != "" && !strings.Contains(string(output), c.host) {
				said := regexp.MustCompile(`(?m)^  host: .*$`).FindString(string(output))
				if said == "" {
					t.Fatalf("the spec does not say what this host does (want %q):\n%s", c.host, output)
				}
				t.Skipf("SKIPPING, NOT PASSING: the spec says %q here, not %q", strings.TrimSpace(said), c.host)
			}
			if err == nil || !strings.Contains(string(output), c.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", c.want, err, output)
			}
		})
	}
}
