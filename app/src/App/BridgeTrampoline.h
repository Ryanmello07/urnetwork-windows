// A wallet bridge url too long for ShellExecuteW, and the page of our own that
// opens it instead.
//
// Why it exists: ShellExecuteW hands a url on to the browser only up to
// INTERNET_MAX_URL_LENGTH (2,083 characters), and Chromium refuses external
// opens past 2,048. The payout consent's bridge url carries the server's whole
// consent message (in its fragment, v2/CONSENT.md §10), about 2,700 characters
// for the protocol's own fixture: opened as it is, it would reach the page cut
// short, without the parameters the page needs, and the wallet could never
// answer. A url over kDirectUrlLimit is written instead into a small page in
// the user's own temp directory, which only its owner may open; the page
// replaces itself with the url, and the page is deleted once the browser has
// had time to read it (WalletConnect::OpenThroughTrampoline).
//
// The page opens in the browser Windows opens https links with, which is the
// one that holds the user's wallet extension: started by the page's own .html
// association it would often be another (Windows keeps .html with Edge, or an
// editor, unless the user changes it separately), and there the bridge page
// finds no Talisman. So ShellExecuteW starts the https program on the page's
// short path, and the .html association only when Windows names no https
// program or it does not start.
//
// Pure and header-only, so tools/bridge-trampoline-tests.cpp runs it on any
// host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::bridge {

// The longest url opened as it is: under Chromium's 2,048 and ShellExecuteW's
// 2,083, with room to spare.
inline constexpr size_t kDirectUrlLimit = 2000;

// How long a page stays for the browser to read it before it is deleted.
inline constexpr std::chrono::seconds kTrampolineLifetime{60};

// A page's file name in the temp directory is this prefix, a random part and
// .html; a flow's page first deletes every page an earlier flow left.
inline constexpr std::wstring_view kTrampolinePrefix = L"urnetwork-bridge-";
inline constexpr std::wstring_view kTrampolinePattern = L"urnetwork-bridge-*.html";

inline bool NeedsTrampoline(std::string_view url) { return url.size() > kDirectUrlLimit; }

// A url character (RFC 3986's unreserved and reserved characters, and the
// percent sign): none of them can end the page's script string or its script.
constexpr bool IsUrlCharacter(char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
  return std::string_view("-._~:/?#[]@!$&'()*+,;=%").find(c) != std::string_view::npos;
}

// The page that opens `url`: it replaces itself with the url, sends no
// referrer, and holds nothing else. Only an https url of url characters (the
// bridge urls are the SDK's, percent-encoded); "" for anything else, which is
// then never opened.
inline std::string TrampolinePage(std::string_view url) {
  if (!url.starts_with("https://")) return std::string();
  for (const char c : url) {
    if (!IsUrlCharacter(c)) return std::string();
  }
  std::string page =
      "<!doctype html>\n"
      "<meta charset=\"utf-8\">\n"
      "<meta name=\"referrer\" content=\"no-referrer\">\n"
      "<title>URnetwork</title>\n"
      "<script>location.replace(\"";
  page += url;
  page += "\");</script>\n";
  return page;
}

// One ShellExecuteW "open": the file it starts and that file's parameters
// ("" for none).
struct TrampolineLaunch {
  std::wstring file;
  std::wstring parameters;

  bool operator==(const TrampolineLaunch&) const = default;
};

// How the page at `pagePath` is opened, in the order to try: first by
// `httpsProgram`, the program Windows opens https links with ("" when it names
// none), given the page's path as its one, quoted argument; then the page
// itself, which starts whatever opens .html.
inline std::vector<TrampolineLaunch> LaunchesFor(std::wstring_view httpsProgram,
                                                 std::wstring_view pagePath) {
  std::vector<TrampolineLaunch> launches;
  if (!httpsProgram.empty()) {
    launches.push_back({std::wstring(httpsProgram), L"\"" + std::wstring(pagePath) + L"\""});
  }
  launches.push_back({std::wstring(pagePath), std::wstring()});
  return launches;
}

}  // namespace urnw::bridge
