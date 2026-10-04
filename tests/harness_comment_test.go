// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// A backslash at the end of a line splices the next line onto it before
// comments are recognized, so a // comment that ends in one swallows the
// line after it. GCC reports that as -Wcomment ("multi-line comment"), and
// the harnesses here compile with -Wall -Wextra -Werror. That turned upstream
// CI red for 20 runs, from usage comments that wrapped a shell command with
// a trailing backslash. Each harness copies the last one's header, so the
// pattern spreads unless something refuses it.

// continuedLineComment reports whether line holds a // comment, outside any
// string or character literal and outside a /* */ comment, that ends in a
// backslash (trailing spaces, tabs and a CR are ignored, as the compiler
// ignores them). inBlock carries the /* */ state from line to line. Raw
// string literals are not tracked; no harness uses one.
func continuedLineComment(line string, inBlock *bool) bool {
	trimmed := strings.TrimRight(line, " \t\r")
	inString, inChar := false, false
	for index := 0; index < len(trimmed); index++ {
		character := trimmed[index]
		next := byte(0)
		if index+1 < len(trimmed) {
			next = trimmed[index+1]
		}
		switch {
		case *inBlock:
			if character == '*' && next == '/' {
				*inBlock = false
				index++
			}
		case inString:
			if character == '\\' {
				index++
			} else if character == '"' {
				inString = false
			}
		case inChar:
			if character == '\\' {
				index++
			} else if character == '\'' {
				inChar = false
			}
		case character == '"':
			inString = true
		case character == '\'':
			inChar = true
		case character == '/' && next == '*':
			*inBlock = true
			index++
		case character == '/' && next == '/':
			return strings.HasSuffix(trimmed, `\`)
		}
	}
	return false
}

func TestContinuedLineCommentDetector(t *testing.T) {
	for _, tc := range []struct {
		lines []string
		want  []bool
	}{
		// the shape that broke CI, with LF and with CRLF line endings
		{[]string{`//   c++ -std=c++20 -I ../src/App url-query-tests.cpp \`, `//       -o /tmp/x`}, []bool{true, false}},
		{[]string{"//   c++ -I ../src/App x.cpp \\\r"}, []bool{true}},
		{[]string{"int x = 1;  // trailing \\  \t"}, []bool{true}},
		{[]string{`// ends in a path C:\`}, []bool{true}},
		// not a // comment that ends in a backslash
		{[]string{`// an ordinary comment`, `#define TWO_LINES(a) \`, `  (a)`}, []bool{false, false, false}},
		{[]string{`const char* url = "http://example.com/" \`}, []bool{false}},
		{[]string{`const char* s = "a \" // not a comment \`}, []bool{false}},
		{[]string{`char c = '"'; // quote \`}, []bool{true}},
		{[]string{`/* block // still a block \`, `   still a block */ int y; // real \`}, []bool{false, true}},
		{[]string{`// \ in the middle only`}, []bool{false}},
	} {
		inBlock := false
		for index, line := range tc.lines {
			if got := continuedLineComment(line, &inBlock); got != tc.want[index] {
				t.Errorf("%q (line %d of %q): got %v, want %v", line, index+1, tc.lines, got, tc.want[index])
			}
		}
	}
}

func TestHarnessCommentsDoNotEndInABackslash(t *testing.T) {
	root := repositoryRoot(t)
	files, err := filepath.Glob(filepath.Join(root, "app", "tools", "*.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	// A glob that found nothing would pass this test without checking
	// anything: there are dozens of harnesses.
	if len(files) < 20 {
		t.Fatalf("found only %d app/tools/*.cpp files", len(files))
	}
	sort.Strings(files)
	var offenders []string
	for _, file := range files {
		data, err := os.ReadFile(file)
		if err != nil {
			t.Fatal(err)
		}
		inBlock := false
		for number, line := range strings.Split(string(data), "\n") {
			if continuedLineComment(line, &inBlock) {
				offenders = append(offenders, filepath.Base(file)+":"+strconv.Itoa(number+1))
			}
		}
	}
	t.Logf("scanned %d app/tools/*.cpp files", len(files))
	if len(offenders) != 0 {
		t.Errorf("%d // comment line(s) end in a backslash, which -Werror=comment refuses; "+
			"put each command on one line: %s", len(offenders), strings.Join(offenders, ", "))
	}
}
