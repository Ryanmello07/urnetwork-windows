// Executable spec for the Settings link to the ur.io cloud proxies page
// (App/CloudProxyLink.h): the app has no protocol switch, so WireGuard, SOCKS
// and HTTPS proxies are created on ur.io. The link is the official page, it
// carries no credential, and the Connections section opens it - run against
// the SAME header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App cloud-proxy-link-tests.cpp -o /tmp/cloud-proxy-link-tests && /tmp/cloud-proxy-link-tests
//
// Run from tools/: the wiring case reads ../src/App/SettingsPage.cpp (or the
// path given as the first argument).
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "CloudProxyLink.h"

using namespace urnw::cloudproxy;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string settingsPath = 1 < argc ? argv[1] : std::string("../src/App/SettingsPage.cpp");

  {
    TEST_CASE("the link is the proxies page on ur.io");
    Check(kProxiesUrl == L"https://ur.io/app/proxies", "the proxies page url");
    Check(kProxiesUrl.starts_with(L"https://ur.io/"), "https on the official ur.io host");
  }
  {
    TEST_CASE("the link carries no credential");
    // never a token or an auth code in the url: ur.io signs the user in itself
    Check(kProxiesUrl.find(L'?') == std::wstring_view::npos, "no query");
    Check(kProxiesUrl.find(L'#') == std::wstring_view::npos, "no fragment");
    Check(kProxiesUrl.find(L'@') == std::wstring_view::npos, "no user info");
  }
  {
    TEST_CASE("the Connections section opens the link");
    const std::string source = ReadFile(settingsPath);
    Check(!source.empty(), "read " + settingsPath);
    Check(source.find("cloudproxy::kProxiesUrl") != std::string::npos,
          "SettingsPage.cpp launches cloudproxy::kProxiesUrl");
    Check(source.find("Loc(\"use_wireguard_socks_https_proxy\")") != std::string::npos,
          "the row is labelled use_wireguard_socks_https_proxy");
    Check(source.find("Loc(\"use_wireguard_socks_https_proxy_note\")") != std::string::npos,
          "the row notes use_wireguard_socks_https_proxy_note");
    Check(source.find("OpenCloudProxies()") != std::string::npos,
          "a row click opens the cloud proxies page");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
