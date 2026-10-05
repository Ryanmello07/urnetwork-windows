// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Seeker token verification is offered only by the Android Solana dApp Store
// build (the solana_dapp flavor); the Windows app offers none, as the Linux app
// offers none. No verify button, wallet picker, wallet signature or
// verifySeekerHolder call, and no wallet lookup for the holder flag: a network
// verified on Android still sees its multiplier row in the points breakdown,
// shown from the points themselves (a payout_multiplier event). The retired
// seeker_points_only ("applies to points only") understated what the token
// gives (it also doubles the free daily and referral data), so no app source
// may show it again.
func TestNoSeekerVerification(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	entries, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		name := entry.Name()
		ext := filepath.Ext(name)
		if entry.IsDir() ||
			(ext != ".cpp" && ext != ".h" && ext != ".xaml" && ext != ".idl" && ext != ".vcxproj") {
			continue
		}
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		for _, dead := range []string{
			"verifySeekerHolder", "VerifySeekerNftHolder", "Verify Seeker Token Holder",
			"OnVerifySeeker", "VerifySeekerButton", "SeekerVerifyNotice", "has_seeker_token",
			"verify_seeker", "confirm_seeker_token", "connect_seeker_wallet",
		} {
			if strings.Contains(string(source), dead) {
				t.Errorf("%s: still has %s; Seeker verification is only in the Android Solana dApp Store build", name, dead)
			}
		}
		if strings.Contains(string(source), `"seeker_points_only"`) {
			t.Errorf("%s shows seeker_points_only; the Seeker token also doubles free and referral data", name)
		}
	}

	// what stays: the multiplier row, gated on the points breakdown alone
	if !strings.Contains(readAppSource(t, "WalletPage.cpp"), "if (accountPoints_.multiplier > 0)") {
		t.Error("WalletPage.cpp: the multiplier row does not follow the points breakdown's multiplier")
	}
}
