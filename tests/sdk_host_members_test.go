// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// A guard over SdkHost.h's data members, which only a Windows build compiles.

// SdkHost.h cannot be compiled off Windows, and it is where most changes add
// state, so two of them can each add a data member under one name and merge
// cleanly into a class that does not compile: the balance recovery's
// disconnect observer was once declared as userDisconnected_, the name of the
// session worker's own fact. Every data member SdkHost's own body declares has
// a name of its own. Members of a nested type and locals of an inline function
// are not SdkHost's; a declaration split over lines is read by its last line.
func TestSdkHostDeclaresEachMemberOnce(t *testing.T) {
	header := stripComments(readAppSource(t, "SdkHost.h"))
	start := strings.Index(header, "\nclass SdkHost {")
	if start < 0 {
		t.Fatal("SdkHost.h no longer declares class SdkHost; update this contract")
	}
	literal := regexp.MustCompile(`"(\\.|[^"\\])*"|'(\\.|[^'\\])*'`)
	member := regexp.MustCompile(`^\s*[\w:<>,\s*&\[\]()]*[\w>*&\])]\s+(\w+_)\s*(\{[^;]*\}|=[^;]*|\[[^;]*\])?;\s*$`)
	// SdkHost.h's line number of the class's first line, less one
	offset := strings.Count(header[:start+1], "\n")
	declared := map[string]int{}
	depth := 0
	for index, line := range strings.Split(header[start+1:], "\n") {
		line = literal.ReplaceAllString(line, `""`)
		trimmed := strings.TrimSpace(line)
		if depth == 1 && !strings.HasPrefix(trimmed, "using ") && !strings.HasPrefix(trimmed, "typedef ") {
			if match := member.FindStringSubmatch(line); match != nil {
				if first, seen := declared[match[1]]; seen {
					t.Errorf("class SdkHost declares %s twice (SdkHost.h lines %d and %d)",
						match[1], first, offset+index+1)
				} else {
					declared[match[1]] = offset + index + 1
				}
			}
		}
		depth += strings.Count(line, "{") - strings.Count(line, "}")
		if depth == 0 && index > 0 {
			break
		}
	}
	// the class's state is well over a hundred members: far fewer means this
	// stopped reading it, and would pass a header it cannot see
	if len(declared) < 100 {
		t.Fatalf("read only %d data members of class SdkHost; update this contract", len(declared))
	}
}
