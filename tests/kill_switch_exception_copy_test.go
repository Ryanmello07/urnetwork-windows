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

// The kill switch exception disclosure must match what the tunnel installs.
// NetworkConfig::Apply routes net::kTunCaptureV6 (::/0 minus link-local, ULA
// and multicast) into the tun, so the disclosure must not tell users that IPv6
// bypasses the VPN (it said so after the dual-stack tunnel landed). SMTP on TCP
// port 25 is still a deliberate local route (connect ip_smtp_policy.go).

const killSwitchExceptionKey = "kill_switch_exception_smtp_detail"

func TestKillSwitchExceptionTunnelCapturesPublicIpv6(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("kill switch copy tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	dir := t.TempDir()
	program := `#include "NetPolicy.h"
#include <cstdio>
int main() {
  using namespace urnw::net;
  // 2606:4700:4700::1111 and 2a00:1450:4001:80b::200e
  const bool publicBypass =
      IsLocalBypassV6(0x2606'4700'4700'0000ull, 0x0000'0000'0000'1111ull) ||
      IsLocalBypassV6(0x2a00'1450'4001'080bull, 0x0000'0000'0000'200eull);
  const bool ulaBypass = IsLocalBypassV6(0xfd00'7572'6e65'0001ull, 1);
  if (publicBypass || !ulaBypass) {
    std::printf("public=%d ula=%d\n", publicBypass, ulaBypass);
    return 1;
  }
  return 0;
}
`
	source := filepath.Join(dir, "main.cpp")
	if err := os.WriteFile(source, []byte(program), 0600); err != nil {
		t.Fatal(err)
	}
	binary := filepath.Join(dir, "kill-switch-routes")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Service"), source, "-o", binary)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build failed: %v\n%s", err, output)
	}
	if output, err := exec.Command(binary).CombinedOutput(); err != nil {
		t.Fatalf("public IPv6 is not captured by the tun: %v\n%s", err, output)
	}
}

func TestKillSwitchExceptionDoesNotSayIpv6BypassesTheVpn(t *testing.T) {
	root := repositoryRoot(t)
	page, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SettingsPage.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(page)
	if strings.Contains(text, "IPv6 is not routed through URnetwork") {
		t.Error("SettingsPage.cpp still says IPv6 is not routed through URnetwork")
	}
	if !strings.Contains(text, `"`+killSwitchExceptionKey+`"`) {
		t.Errorf("SettingsPage.cpp does not show %s", killSwitchExceptionKey)
	}

	english := reswValue(t, root, "en", killSwitchExceptionKey)
	if english == "" {
		t.Fatalf("en/Resources.resw has no %s", killSwitchExceptionKey)
	}
	if !strings.Contains(english, "IPv6 is routed through URnetwork like IPv4") {
		t.Errorf("%s does not say IPv6 is routed through the VPN: %q", killSwitchExceptionKey, english)
	}
	// the port 25 exception is real and stays disclosed
	if !strings.Contains(english, "SMTP on TCP port 25 bypasses the VPN") {
		t.Errorf("%s no longer discloses the SMTP port 25 exception: %q", killSwitchExceptionKey, english)
	}
}

func TestKillSwitchExceptionIsLocalized(t *testing.T) {
	root := repositoryRoot(t)
	for _, locale := range []string{"ar", "de", "es", "ru", "zh-Hans"} {
		if reswValue(t, root, locale, killSwitchExceptionKey) == "" {
			t.Errorf("%s translation of %s missing", locale, killSwitchExceptionKey)
		}
	}
}

func reswValue(t *testing.T, root string, locale string, name string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Strings", locale, "Resources.resw"))
	if err != nil {
		t.Fatal(err)
	}
	pattern := regexp.MustCompile(`(?s)<data name="` + regexp.QuoteMeta(name) + `" xml:space="preserve">\s*<value>(.*?)</value>`)
	match := pattern.FindSubmatch(data)
	if match == nil {
		return ""
	}
	return string(match[1])
}
