// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the Manage Subscription visibility spec
// (App/ManageSubscription.h), and check the row follows the balance's
// subscription store: hidden until a Stripe subscription shows.
func TestManageSubscriptionOnlyForStripe(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("manage subscription tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "manage-subscription-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "manage-subscription-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build manage subscription tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("manage subscription: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	for name, wants := range map[string][]string{
		"SubscriptionBalance.cpp": {"urnet::classifySubscriptionStore(result.current_subscription->store)"},
		"SettingsPage.cpp": {
			"manageSubscription_.Visibility(Visibility::Collapsed)",
			"urnw::ShowsManageSubscription(storeFamily)",
		},
		"MainWindow.xaml.cpp": {"settings_->ApplySubscriptionStore(balance_.subscriptionStoreFamily)"},
	} {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		for _, want := range wants {
			if !strings.Contains(string(source), want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}
}
