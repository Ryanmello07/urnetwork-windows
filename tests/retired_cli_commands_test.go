// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

// No catalog string may tell a provider to run a CLI command that no longer
// exists. Head binding moved from "provider bind-head" / "snclaim bind-head" to
// "provider fleet bind"; the Top 200 hint kept naming the old one in every
// locale (support inbox 7934).
func TestNoCatalogStringNamesARetiredCliCommand(t *testing.T) {
	retired := regexp.MustCompile(`\b(un)?bind-head\b`)
	root := repositoryRoot(t)
	catalogs, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil || len(catalogs) == 0 {
		t.Fatalf("no catalogs found: %v", err)
	}
	for _, catalog := range catalogs {
		b, err := os.ReadFile(catalog)
		if err != nil {
			t.Fatal(err)
		}
		if match := retired.Find(b); match != nil {
			rel, _ := filepath.Rel(root, catalog)
			t.Errorf("%s names the retired command %q", rel, match)
		}
	}
}
