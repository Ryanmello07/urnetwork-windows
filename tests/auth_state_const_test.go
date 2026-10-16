// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// CurrentTrayFacts is const. Compile the real getter declaration and body
// through the same const receiver, then check that it observes both auth states.
// A source-string assertion missed the missing qualifier until the MSVC build.
func TestCachedAuthStateIsReadableThroughConstHost(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("cached auth state tests require a C++20 compiler: ", err)
	}
	header := stripComments(readAppSource(t, "SdkHost.h"))
	declaration := regexp.MustCompile(`\bbool IsLoggedIn\(\)[^;]*;`).FindString(header)
	if declaration == "" {
		t.Fatal("SdkHost.h no longer declares IsLoggedIn")
	}
	body := regexp.MustCompile(`\bbool SdkHost::IsLoggedIn\(\)[^{]*\{[^}]*\}`).FindString(
		stripComments(readAppSource(t, "SdkHost.cpp")))
	if body == "" {
		t.Fatal("SdkHost.cpp no longer defines the cached IsLoggedIn getter")
	}
	source := "#include <atomic>\nstruct SdkHost {\n" + declaration + `
  std::atomic<bool> loggedIn_{false};
};
` + body + `
int main() {
  SdkHost host;
  const SdkHost& view = host;
  if (view.IsLoggedIn()) return 1;
  host.loggedIn_.store(true, std::memory_order_release);
  if (!view.IsLoggedIn()) return 2;
  host.loggedIn_.store(false, std::memory_order_release);
  return view.IsLoggedIn() ? 3 : 0;
}
`
	dir := t.TempDir()
	cpp := filepath.Join(dir, "auth-state-const.cpp")
	program := filepath.Join(dir, "auth-state-const")
	if err := os.WriteFile(cpp, []byte(source), 0600); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		cpp, "-o", program).CombinedOutput(); err != nil {
		t.Fatalf("compile the cached auth getter through a const host: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("cached auth states: %v\n%s", err, output)
	}
	// The original non-const declaration and definition must fail specifically
	// at the const receiver, not because the extracted fixture is incomplete.
	retired := strings.ReplaceAll(source, "IsLoggedIn() const", "IsLoggedIn()")
	if retired == source {
		t.Fatal("the non-const control did not change the real getter")
	}
	if err := os.WriteFile(cpp, []byte(retired), 0600); err != nil {
		t.Fatal(err)
	}
	output, err := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		cpp, "-o", program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), "const SdkHost") ||
		!strings.Contains(string(output), "IsLoggedIn") {
		t.Fatalf("non-const getter control did not reproduce the tray compile failure: %v\n%s", err, output)
	}
}
