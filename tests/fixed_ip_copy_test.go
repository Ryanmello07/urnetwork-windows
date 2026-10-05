// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Fixed IP keeps one exit for the session (connect stickyExit: a Fixed IP
// window has no standing spare and is never drained on the hourly lifetime),
// so the connect options' Fixed IP row says so in a note under its title
// (fixed_ip_subtitle, from the localizations store).

const fixedIpSubtitleKey = "fixed_ip_subtitle"

func TestFixedIpSubtitleCopy(t *testing.T) {
	root := repositoryRoot(t)
	english := reswValue(t, root, "en", fixedIpSubtitleKey)
	if english != "Keeps one exit for the session; changes only if that provider goes offline." {
		t.Fatalf("en/Resources.resw %s = %q", fixedIpSubtitleKey, english)
	}
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if !entry.IsDir() || entry.Name() == "en" {
			continue
		}
		value := reswValue(t, root, entry.Name(), fixedIpSubtitleKey)
		if value == "" {
			t.Errorf("%s translation of %s missing", entry.Name(), fixedIpSubtitleKey)
		} else if value == english {
			t.Errorf("%s translation of %s is English", entry.Name(), fixedIpSubtitleKey)
		}
	}
}

func TestFixedIpRowShowsTheSubtitle(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	read := func(name string) string {
		source, err := os.ReadFile(filepath.Join(appDir, name))
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}

	page := read("ConnectPage.cpp")
	if !strings.Contains(page, `w_.FixedIpNote().Text(Loc("`+fixedIpSubtitleKey+`"));`) {
		t.Errorf("ConnectPage.cpp does not set the Fixed IP note from %s", fixedIpSubtitleKey)
	}

	// the note sits in the Fixed IP row, between its title and its switch
	window := read("MainWindow.xaml")
	label := strings.Index(window, `x:Name="FixedIpLabel"`)
	note := strings.Index(window, `x:Name="FixedIpNote"`)
	toggle := strings.Index(window, `x:Name="FixedIpToggle"`)
	if label < 0 || toggle < 0 {
		t.Fatal("MainWindow.xaml has no Fixed IP row")
	}
	if note < label || toggle < note {
		t.Errorf("MainWindow.xaml: FixedIpNote is not in the Fixed IP row (label %d, note %d, toggle %d)",
			label, note, toggle)
	}
}
