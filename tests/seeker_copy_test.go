// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The Seeker multiplier note must say what a verified token gives: 2x points
// AND 2x the free daily and referral data grants (server pro.yml
// seeker.data_multiplier, subsidy seeker_holder_multiplier). The retired
// seeker_points_only ("applies to points only") understated it, so no app
// source may show it again, and the wallet page must show the benefit.
func TestSeekerMultiplierCopy(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	entries, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		name := entry.Name()
		ext := filepath.Ext(name)
		if entry.IsDir() || (ext != ".cpp" && ext != ".h" && ext != ".xaml") {
			continue
		}
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(source), `"seeker_points_only"`) {
			t.Errorf("%s shows seeker_points_only; the Seeker token also doubles free and referral data", name)
		}
	}

	wallet, err := os.ReadFile(filepath.Join(appDir, "WalletPage.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(wallet), `Loc("seeker_multiplier_benefit")`) {
		t.Error("WalletPage.cpp does not show seeker_multiplier_benefit")
	}

	resw, err := os.ReadFile(filepath.Join(appDir, "Strings", "en", "Resources.resw"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(resw), `<data name="seeker_multiplier_benefit"`) {
		t.Error("the en resources carry no seeker_multiplier_benefit; regenerate from the store")
	}
}
