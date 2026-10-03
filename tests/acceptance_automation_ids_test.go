// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"encoding/xml"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The insufficient-balance acceptance driver finds the app's controls by UI
// Automation id and counts the out-of-balance notice from the app log. These
// are the ids and the log line it depends on; a rename here must move the
// driver with it.
func TestInsufficientBalanceAcceptanceAutomationIds(t *testing.T) {
	root := repositoryRoot(t)
	xamlPath := filepath.Join(root, "app", "src", "App", "MainWindow.xaml")
	nameAutomationIds := map[string]string{
		"EmailBox":         "acceptance.password.user",
		"GetStartedButton": "acceptance.password.next",
		"PasswordBox":      "acceptance.password.input",
		"SignInButton":     "acceptance.password.submit",
		"ConnectNavItem":   "acceptance.nav.connect",
		"SettingsNavItem":  "acceptance.nav.settings",
		"ConnectButton":    "acceptance.connect",
		"BalanceWarning":   "acceptance.insufficient-balance.alert",
	}
	foundAutomationIds := xamlAutomationIds(t, xamlPath)
	for name, automationId := range nameAutomationIds {
		if got := foundAutomationIds[name]; got != automationId {
			t.Errorf("%s: AutomationProperties.AutomationId = %q, want %q", name, got, automationId)
		}
	}

	sourceSnippets := map[string]string{
		filepath.Join("app", "src", "App", "MainWindow.xaml.cpp"): `L"acceptance.insufficient-balance.upgrade"`,
		filepath.Join("app", "src", "App", "SettingsPage.cpp"):    `L"acceptance.settings.kill-switch"`,
		filepath.Join("app", "src", "App", "AppController.cpp"):   `LogInfo("app: insufficient balance notice posted");`,
	}
	for path, snippet := range sourceSnippets {
		source, err := os.ReadFile(filepath.Join(root, path))
		if err != nil {
			t.Fatal(err)
		}
		if strings.Count(string(source), snippet) != 1 {
			t.Errorf("%s: want exactly one %s", path, snippet)
		}
	}
}

// Maps each x:Name to its AutomationProperties.AutomationId. Parsing the whole
// document also proves the edited XAML is well-formed.
func xamlAutomationIds(t *testing.T, path string) map[string]string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	nameAutomationIds := map[string]string{}
	decoder := xml.NewDecoder(file)
	for {
		token, err := decoder.Token()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		start, ok := token.(xml.StartElement)
		if !ok {
			continue
		}
		name, automationId := "", ""
		for _, attribute := range start.Attr {
			switch {
			case attribute.Name.Local == "Name" && strings.HasSuffix(attribute.Name.Space, "/xaml"):
				name = attribute.Value
			case attribute.Name.Local == "AutomationProperties.AutomationId":
				automationId = attribute.Value
			}
		}
		if name != "" && automationId != "" {
			nameAutomationIds[name] = automationId
		}
	}
	return nameAutomationIds
}
