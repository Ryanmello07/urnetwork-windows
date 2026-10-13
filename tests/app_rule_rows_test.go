// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The App split rules sheet's list (App/AppRuleRows.h): one row per app,
// however many rules name it and in whatever case. Two rules whose paths
// differ only in case put one cached row in the sheet's wanted list twice,
// its reconcile then inserted past the end of the children (E_BOUNDS), and the
// sheet never opened. The spec runs the header; the sheet's wiring needs WinRT
// and is read from its source with comments stripped.

// Compile and execute the list spec (app/tools/app-rule-rows-tests.cpp).
func TestAppRuleRows(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("app rule rows tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "app-rule-rows-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "app-rule-rows-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build app rule rows tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("app rule rows: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The sheet runs the spec's code: its ruled and unruled apps come from the
// header, its children are reconciled by the header's Reconcile and by nothing
// else, and its one key function is the header's RowKey.
func TestAppRuleRowsTheSheetRunsThem(t *testing.T) {
	source := stripComments(lf(readAppSource(t, "StatsSheets.cpp")))
	if !strings.Contains(source, `#include "AppRuleRows.h"`) {
		t.Error(`StatsSheets.cpp does not include "AppRuleRows.h"`)
	}
	key := definitionBody(t, "StatsSheets.cpp", source, "static std::string LowerAscii(std::string s)")
	provideRequire(t, "LowerAscii", key, "return apprules::RowKey(std::move(s));")

	render := definitionBody(t, "StatsSheets.cpp", source, "void AppRulesSheet::RenderList() {")
	provideRequire(t, "AppRulesSheet::RenderList", render,
		"const std::vector<InstalledApp> ruled = apprules::RuledApps(rules, installed_);",
		"const std::vector<InstalledApp> unruled = apprules::UnruledApps(rules, installed_);",
		"apprules::Reconcile(appsList_.Children(), want,")
	// nothing else in the render moves the list's children: a second, inline
	// reconcile would be code the spec does not run
	for _, mutation := range []string{".InsertAt(", ".RemoveAt(", ".Append(", ".Clear()"} {
		if strings.Contains(render, mutation) {
			t.Errorf("AppRulesSheet::RenderList changes the list's children itself (%s); go through apprules::Reconcile", mutation)
		}
	}
	// ...and no other function adds to the list
	for _, line := range strings.Split(source, "\n") {
		if strings.Contains(line, "appsList_.Children().") && !strings.Contains(line, "apprules::Reconcile(appsList_.Children(), want,") {
			t.Errorf("StatsSheets.cpp changes the app list's children outside RenderList's reconcile: %s", strings.TrimSpace(line))
		}
	}

	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="AppRuleRows.h" />`) {
		t.Error("App.vcxproj does not list AppRuleRows.h")
	}
}
