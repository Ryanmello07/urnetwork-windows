// Executable spec for the "Fast DNS on connect" DNS setting
// (App/FastDnsOnConnect.h): the opt-in host-network DNS fallback, off by
// default. Runs on any host with a C++20 compiler, against a stand-in for
// urnet::DnsResolverSettings, the app sources and the generated English
// strings, so it needs no SDK build and no Windows:
//
//   c++ -std=c++20 -I ../src/App dns-settings-tests.cpp \
//       -o /tmp/dns-settings-tests && /tmp/dns-settings-tests ..
//
// The argument is the app directory (default ".."). The checks:
//  - the toggle maps to and from EnableFallback unchanged, and shows off when
//    there are no applied settings to read;
//  - the DNS editor toggle, its description and the drawer status row use the
//    fast_dns_on_connect keys, not the retired "Local DNS fallback" wording;
//  - those keys exist in the generated English strings and the retired ones
//    are gone.
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "FastDnsOnConnect.h"

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Case(const std::string& name) {
  ++gCases;
  gCurrentCase = name;
}

// the fields of urnet::DnsResolverSettings the mapping touches
struct StandInSettings {
  bool EnableRemoteDoh = false;
  bool EnableFallback = false;
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Fail("cannot read " + path);
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string appDir = 1 < argc ? argv[1] : "..";
  namespace fast = urnw::fast_dns_on_connect;

  Case("no applied settings shows the toggle off");
  if (fast::FromSettings(std::optional<StandInSettings>{})) {
    Fail("absent settings read as fast dns on");
  }

  Case("the toggle reads EnableFallback unchanged");
  for (bool enabled : {false, true}) {
    StandInSettings settings;
    settings.EnableRemoteDoh = true;
    settings.EnableFallback = enabled;
    if (fast::FromSettings(std::optional<StandInSettings>{settings}) != enabled) {
      Fail(std::string("EnableFallback=") + (enabled ? "true" : "false") + " read back flipped");
    }
  }

  Case("the toggle writes EnableFallback unchanged");
  for (bool enabled : {false, true}) {
    StandInSettings settings;
    settings.EnableFallback = !enabled;
    fast::ToSettings(settings, enabled);
    if (settings.EnableFallback != enabled) {
      Fail(std::string("toggle ") + (enabled ? "on" : "off") + " not written to EnableFallback");
    }
  }

  Case("the toggle is labeled Fast DNS on connect");
  if (fast::kLabelKey != "fast_dns_on_connect" ||
      fast::kDescriptionKey != "fast_dns_on_connect_description") {
    Fail("unexpected label keys");
  }

  Case("the editor and the drawer use the fast dns keys");
  for (const char* source : {"/src/App/StatsSheets.cpp", "/src/App/ConnectPage.cpp"}) {
    const std::string text = ReadFile(appDir + source);
    if (Contains(text, "\"local_dns_fallback")) {
      Fail(std::string(source) + " still shows the retired Local DNS fallback wording");
    }
    if (!Contains(text, "fast_dns_on_connect::kLabelKey")) {
      Fail(std::string(source) + " does not label the setting Fast DNS on connect");
    }
  }
  if (!Contains(ReadFile(appDir + "/src/App/StatsSheets.cpp"), "fast_dns_on_connect::kDescriptionKey")) {
    Fail("the editor does not describe what Fast DNS on connect reveals");
  }

  Case("the English strings carry the fast dns keys");
  {
    const std::string resw = ReadFile(appDir + "/src/App/Strings/en/Resources.resw");
    if (!Contains(resw, "<data name=\"fast_dns_on_connect\"") ||
        !Contains(resw, "<value>Fast DNS on connect</value>")) {
      Fail("en Resources.resw has no Fast DNS on connect label");
    }
    if (!Contains(resw, "<data name=\"fast_dns_on_connect_description\"") ||
        !Contains(resw, "reveal your lookups to the local network")) {
      Fail("en Resources.resw has no Fast DNS on connect description");
    }
    if (Contains(resw, "<data name=\"local_dns_fallback\"")) {
      Fail("en Resources.resw still carries the retired local_dns_fallback key");
    }
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " dns settings: " << gCases << " cases, "
            << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
