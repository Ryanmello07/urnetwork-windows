// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Manage Subscription row (UPGRADE.md D5): the Stripe customer portal url
// must be opened by AWAITING the launcher's verdict, and a failed launch must
// say so (site_billing_portal_error) instead of looking like a portal that
// opened. A bare LaunchUriAsync(...) statement discards that verdict.
func TestCustomerPortalLaunchIsObserved(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SettingsPage.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	settings := string(source)

	// every launch in the page is awaited
	launches := regexp.MustCompile(`(?s)(co_await\s+)?winrt::Windows::System::Launcher::LaunchUriAsync\(`).
		FindAllStringSubmatch(settings, -1)
	if len(launches) == 0 {
		t.Fatal("SettingsPage.cpp: no LaunchUriAsync for the customer portal")
	}
	for _, launch := range launches {
		if launch[1] == "" {
			t.Error("SettingsPage.cpp: a LaunchUriAsync result is discarded (fire-and-forget launch)")
		}
	}

	at := strings.Index(settings, "winrt::fire_and_forget SettingsPage::LaunchCustomerPortal(")
	if at < 0 {
		t.Fatal("SettingsPage.cpp: no LaunchCustomerPortal")
	}
	body := settings[at:]
	if end := strings.Index(body, "\n}\n"); end >= 0 {
		body = body[:end]
	}
	for _, want := range []string{"co_await", `Loc("site_billing_portal_error")`} {
		if !strings.Contains(body, want) {
			t.Errorf("LaunchCustomerPortal: missing %s", want)
		}
	}
	if !strings.Contains(settings, "page.LaunchCustomerPortal(url)") {
		t.Error("OpenCustomerPortal does not open the portal through LaunchCustomerPortal")
	}
}
