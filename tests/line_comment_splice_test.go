// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A guard over the app's C++ sources for a line comment that GCC splices into
// the next line.

// A // comment whose line ends in a backslash takes the next line with it: the
// backslash splices the two lines before comments are removed. GCC reports it
// as -Wcomment, an error under the harnesses' -Werror, so `go test ./tests` on
// the CI's Linux runner fails at the build (MSVC reports it too, C4010). Apple
// clang says nothing, so a usage line wrapped with a trailing backslash passes
// every macOS run; this reads the sources, so it fails on any host. Keep a
// usage line on one line, as the harnesses do.
func TestNoLineCommentEndsInBackslash(t *testing.T) {
	root := repositoryRoot(t)
	for _, dir := range []string{
		filepath.Join("app", "src", "App"),
		filepath.Join("app", "src", "Common"),
		filepath.Join("app", "src", "Service"),
		filepath.Join("app", "tools"),
	} {
		entries, err := os.ReadDir(filepath.Join(root, dir))
		if err != nil {
			t.Fatal(err)
		}
		checked := 0
		for _, entry := range entries {
			switch filepath.Ext(entry.Name()) {
			case ".cpp", ".h", ".hpp":
			default:
				continue
			}
			if entry.IsDir() {
				continue
			}
			name := filepath.Join(dir, entry.Name())
			source, err := os.ReadFile(filepath.Join(root, name))
			if err != nil {
				t.Fatal(err)
			}
			checked++
			lines := strings.Split(string(source), "\n")
			code := strings.Split(stripLineComments(string(source)), "\n")
			for index, line := range lines {
				// a backslash, then only blanks (or a CRLF's CR), still splices
				line = strings.TrimRight(line, " \t\r")
				if len(code[index]) < len(line) && strings.HasSuffix(line, `\`) {
					t.Errorf("%s:%d: the // comment ends in a backslash, which comments out the next line too (GCC -Werror=comment); make it one line", name, index+1)
				}
			}
		}
		if checked == 0 {
			t.Errorf("%s: no C++ sources found; update this check", dir)
		}
	}
}
