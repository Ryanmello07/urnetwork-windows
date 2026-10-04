// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// UPGRADE.md §4.4: the checkout bridge url and its urnetwork://checkout
// hand-back are the SDK's envelope (urnet::buildInlineCheckoutBridgeUrl,
// urnet::isCheckoutRedirect, urnet::parseCheckoutRedirect), not a hand-built
// copy, and the app keeps ONE percent-encoder (App/UrlQuery.h) for the urls it
// still builds itself. Compiles and runs the encoder spec, then reads the
// sources.
func TestCheckoutEnvelopeAndOneEncoder(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("url query tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "url-query-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir, filepath.Join(root, "app", "tools", "url-query-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build url query tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("url query: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	// one percent-encoder in the app
	entries, err := os.ReadDir(appDir)
	if err != nil {
		t.Fatal(err)
	}
	var encoders []string
	for _, entry := range entries {
		name := entry.Name()
		if !strings.HasSuffix(name, ".cpp") && !strings.HasSuffix(name, ".h") {
			continue
		}
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(source), `"0123456789ABCDEF"`) {
			encoders = append(encoders, name)
		}
	}
	if len(encoders) != 1 || encoders[0] != "UrlQuery.h" {
		t.Errorf("percent-encoders: want only UrlQuery.h, got %v", encoders)
	}

	source, err := os.ReadFile(filepath.Join(appDir, "BalanceSheets.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	sheets := string(source)
	for _, want := range []string{
		"urnet::buildInlineCheckoutBridgeUrl(clientSecret)",
		"urnet::isCheckoutRedirect(uri)",
		"urnet::parseCheckoutRedirect(uri)",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}
	for _, unwanted := range []string{`"https://ur.io/checkout"`, `"urnetwork://checkout"`, `status->second == "complete"`} {
		if strings.Contains(sheets, unwanted) {
			t.Errorf("BalanceSheets.cpp: still hand-builds or parses the envelope (%s)", unwanted)
		}
	}
}
