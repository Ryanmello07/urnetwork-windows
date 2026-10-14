// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"os"
	"path/filepath"
	"regexp"
	"strings"
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

// The Top 200 binding steps must name the sign-only `provider fleet sign` that
// prints the payload the page takes, and the hotkey hint the flags that make
// the hotkey signature, verbatim in every locale (support inbox 7934).
func TestTop200BindingStepsNameFleetSign(t *testing.T) {
	required := map[string][]string{
		"site_app_top200_step_bind":        {"provider fleet sign", "~/.urnetwork/.provider.key"},
		"site_app_top200_hotkey_sign_hint": {"provider fleet sign", "--hotkey_seed_file", "--client_id"},
	}
	root := repositoryRoot(t)
	// the site keys ship in the six site locales only
	for _, locale := range []string{"ar", "de", "en", "es", "ru", "zh-Hans"} {
		catalog := filepath.Join(root, "app", "src", "App", "Strings", locale, "Resources.resw")
		b, err := os.ReadFile(catalog)
		if err != nil {
			t.Fatal(err)
		}
		var resources struct {
			Data []struct {
				Name  string `xml:"name,attr"`
				Value string `xml:"value"`
			} `xml:"data"`
		}
		if err := xml.Unmarshal(b, &resources); err != nil {
			t.Fatalf("%s: %v", catalog, err)
		}
		rel, _ := filepath.Rel(root, catalog)
		for id, needles := range required {
			found := false
			for _, data := range resources.Data {
				if data.Name != id {
					continue
				}
				found = true
				for _, needle := range needles {
					if !strings.Contains(data.Value, needle) {
						t.Errorf("%s %s does not name %q", rel, id, needle)
					}
				}
			}
			if !found {
				t.Errorf("%s has no %s", rel, id)
			}
		}
	}
}
