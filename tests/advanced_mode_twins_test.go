// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"regexp"
	"strings"
	"testing"
)

// The Advanced Mode toggle is built twice on the Settings page: in the Device
// pane, and as a fold copy at the foot of General that shows while that pane
// is folded. One apply path, SettingsPage::ApplyAdvancedMode, writes the
// standing value to both. It returned early when the Device pane's copy
// already read the value - and a flip of that copy has always moved it by the
// time the value comes back through the apply path, so the fold copy kept the
// old value, and showed it as soon as the window narrowed. No single instance
// can say whether another still reads the old value, so the apply path reads
// none of them: it writes every instance, under its echo guard. The page needs
// WinRT, so this reads its source with comments stripped.

// The instances: every toggle the page builds with the Advanced Mode label.
var advancedTwinBuild = regexp.MustCompile(`([A-Za-z_][A-Za-z0-9_]*)\s*=\s*ToggleRow\(\s*[^;]*?Adv\("adv_advanced_mode"`)

// What is wrong with the apply path in a SettingsPage.cpp source, if anything.
func advancedTwinViolations(source string) (instances []string, violations []string) {
	code := stripComments(lf(source))
	for _, match := range advancedTwinBuild.FindAllStringSubmatch(code, -1) {
		instances = append(instances, match[1])
	}
	if len(instances) < 2 {
		return instances, []string{fmt.Sprintf("found %d Advanced Mode toggles (%v), want the Device pane's and its fold copy", len(instances), instances)}
	}
	start := strings.Index(code, "void SettingsPage::ApplyAdvancedMode(bool on) {")
	if start < 0 {
		return instances, []string{"SettingsPage.cpp no longer defines SettingsPage::ApplyAdvancedMode(bool on)"}
	}
	end := strings.Index(code[start:], "\n}\n")
	if end < 0 {
		return instances, []string{"cannot find the end of SettingsPage::ApplyAdvancedMode"}
	}
	body := code[start : start+end+2]
	// the only way out before the writes is the not-built guard
	for _, line := range strings.Split(body, "\n") {
		if strings.Contains(line, "return") && strings.TrimSpace(line) != "if (!advancedMode_) return;" {
			violations = append(violations, fmt.Sprintf("the apply path returns early: %s", strings.TrimSpace(line)))
		}
	}
	// it reads no instance's state
	if reads := regexp.MustCompile(`[A-Za-z_][A-Za-z0-9_]*\.IsOn\(\s*\)`).FindAllString(body, -1); len(reads) > 0 {
		violations = append(violations, fmt.Sprintf("the apply path reads an instance's state (%s); no instance can say what another shows", strings.Join(reads, ", ")))
	}
	// and writes every instance, inside the echo guard
	guardOn := strings.Index(body, "applyingAdvancedMode_ = true;")
	guardOff := strings.LastIndex(body, "applyingAdvancedMode_ = false;")
	if guardOn < 0 || guardOff < guardOn {
		return instances, append(violations, "the apply path does not set and clear its echo guard (applyingAdvancedMode_)")
	}
	for _, instance := range instances {
		writes := regexp.MustCompile(regexp.QuoteMeta(instance+".IsOn(on);")).FindAllStringIndex(body, -1)
		if len(writes) == 0 {
			violations = append(violations, fmt.Sprintf("the apply path never writes %s", instance))
		}
		for _, write := range writes {
			if write[0] < guardOn || write[0] > guardOff {
				violations = append(violations, fmt.Sprintf("the apply path writes %s outside its echo guard", instance))
			}
		}
	}
	return instances, violations
}

func TestAdvancedModeAppliesToEveryInstance(t *testing.T) {
	instances, violations := advancedTwinViolations(readAppSource(t, "SettingsPage.cpp"))
	t.Logf("the Advanced Mode toggles: %v", instances)
	for _, violation := range violations {
		t.Error(violation)
	}
}

// The contract fires on the early return this replaced, on a write dropped,
// on a write outside the guard, and on a third instance nobody writes.
func TestAdvancedModeTwinsContractFires(t *testing.T) {
	source := lf(readAppSource(t, "SettingsPage.cpp"))
	guard := "  if (!advancedMode_) return;  // the section is not built yet\n"
	if strings.Count(source, guard) != 1 {
		t.Fatalf("SettingsPage.cpp: want the not-built guard once, as the mutations below expect")
	}
	mutations := []struct{ name, old, new, want string }{
		{"the early return put back", guard, guard + "  if (advancedMode_.IsOn() == on) return;\n", "returns early"},
		{"the fold copy's write dropped", "  if (advancedModeFold_) advancedModeFold_.IsOn(on);\n", "", "never writes advancedModeFold_"},
		{"a write outside the guard", "  if (advancedModeFold_) advancedModeFold_.IsOn(on);\n  applyingAdvancedMode_ = false;\n",
			"  applyingAdvancedMode_ = false;\n  if (advancedModeFold_) advancedModeFold_.IsOn(on);\n", "writes advancedModeFold_ outside its echo guard"},
		{"a third instance", "void SettingsPage::BuildAdvancedFoldSection(Panel const& host) {\n",
			"void SettingsPage::BuildAdvancedFoldSection(Panel const& host) {\n  advancedModeThird_ = ToggleRow(card, Adv(\"adv_advanced_mode\", L\"Advanced mode\"), hstring{});\n",
			"never writes advancedModeThird_"},
	}
	for _, mutation := range mutations {
		if count := strings.Count(source, mutation.old); count != 1 {
			t.Errorf("%s: want the line to mutate once, found it %d times", mutation.name, count)
			continue
		}
		_, violations := advancedTwinViolations(strings.Replace(source, mutation.old, mutation.new, 1))
		joined := strings.Join(violations, "; ")
		if len(violations) == 0 || (mutation.want != "" && !strings.Contains(joined, mutation.want)) {
			t.Errorf("%s: want a violation with %q, got %q", mutation.name, mutation.want, violations)
			continue
		}
		t.Logf("fires (%s): %s", mutation.name, joined)
	}
}
