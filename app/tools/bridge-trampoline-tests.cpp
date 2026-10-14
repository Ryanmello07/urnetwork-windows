// Executable spec for the wallet bridge's trampoline (App/BridgeTrampoline.h):
// which urls ShellExecuteW opens as they are and which go through a page of our
// own, what that page holds (the url, verbatim, and nothing else), that it is
// made only for an https url of url characters, so nothing in the url can end
// its script, and that the page opens in the program Windows opens https links
// with (the browser that holds the wallet's extension) before whatever opens
// .html. Run against the same header the app compiles, on any host with a
// C++20 compiler.
//
// Each argument is a bridge url that must go through the page (the payout
// consent's, which tests/bridge_trampoline_test.go builds from the protocol's
// own consent fixture as the SDK does).
//
// The Win32 half (writing the page for its owner alone, asking Windows for the
// https program, starting it, deleting the page) cannot be built off Windows;
// what is verified here is every decision it makes first.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App bridge-trampoline-tests.cpp -o /tmp/bridge-trampoline-tests && /tmp/bridge-trampoline-tests [url...]
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "BridgeTrampoline.h"

using namespace urnw::bridge;

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

constexpr std::string_view kQuery =
    "https://ur.io/wallet-connect?address=5F3sa2TJAWMqDhXG6jhV4N8ko9SxwGy8TpaNS1repo5EYjQX"
    "&method=signMessage&provider=bittensor&purpose=consent"
    "&redirect_link=urnetwork%3A%2F%2Fbittensor-sign-message&wallet=talisman";

// a bridge url of `length` characters, its message in the fragment
std::string BridgeUrl(size_t length) {
  std::string url(kQuery);
  url += "#message=";
  while (url.size() < length) url += 'Q';
  return url;
}

size_t Count(std::string_view text, std::string_view part) {
  size_t count = 0;
  for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + 1)) {
    ++count;
  }
  return count;
}

// the page for `url`, exactly
std::string PageFor(std::string_view url) {
  return "<!doctype html>\n<meta charset=\"utf-8\">\n<meta name=\"referrer\" "
         "content=\"no-referrer\">\n<title>URnetwork</title>\n<script>location.replace(\"" +
         std::string(url) + "\");</script>\n";
}

}  // namespace

int main(int argc, char** argv) {
  {
    TEST_CASE("a url within the limit opens as it is");
    Check(kDirectUrlLimit < 2048, "under Chromium's external-open limit");
    Check(kDirectUrlLimit < 2083, "under ShellExecuteW's INTERNET_MAX_URL_LENGTH");
    Check(!NeedsTrampoline(BridgeUrl(kQuery.size() + 40)), "a login bridge url's size");
    Check(!NeedsTrampoline(BridgeUrl(kDirectUrlLimit)), "exactly the limit");
    Check(!NeedsTrampoline(""), "nothing");
  }
  {
    TEST_CASE("a longer url goes through the page");
    Check(NeedsTrampoline(BridgeUrl(kDirectUrlLimit + 1)), "one past the limit");
    Check(NeedsTrampoline(BridgeUrl(2'900)), "the consent's size");
  }
  {
    TEST_CASE("the page replaces itself with the url, and holds nothing else");
    const std::string url = BridgeUrl(2'900);
    const std::string page = TrampolinePage(url);
    Check(page == PageFor(url), "the page, exactly");
    Check(Count(page, url) == 1, "the url once, verbatim");
    Check(Count(page, "<script") == 1 && Count(page, "</script>") == 1, "one script");
    Check(Count(page, "no-referrer") == 1, "no referrer");
  }
  {
    TEST_CASE("every url character passes, the fragment's and the query's");
    const std::string url = BridgeUrl(2'100) + "-._~:/?#[]@!$&'()*+,;=%2F";
    Check(TrampolinePage(url) == PageFor(url), "the reserved characters");
  }
  {
    TEST_CASE("only an https url is opened through the page");
    for (const char* other : {"", "http://ur.io/wallet-connect", "javascript:alert(1)",
                              "urnetwork://bittensor-sign-message", "file:///C:/Windows/win.ini",
                              "data:text/html,x", "HTTPS://ur.io/wallet-connect"}) {
      Check(TrampolinePage(other).empty(), std::string("\"") + other + "\"");
    }
  }
  {
    TEST_CASE("nothing in the url can end the page's script");
    for (const char breakout :
         {'"', '<', '>', '\\', ' ', '\n', '\r', '\t', '`', '{', '}', '|', '^'}) {
      const std::string url = BridgeUrl(2'100) + breakout + "x";
      Check(TrampolinePage(url).empty(), std::string("character ") + std::to_string(breakout));
    }
    Check(TrampolinePage(BridgeUrl(2'100) + "\");alert(1);//").empty(), "a closing quote");
    Check(TrampolinePage(BridgeUrl(2'100) + "</script><script>alert(1)").empty(),
          "a closing script tag");
    std::string nul = BridgeUrl(2'100);
    nul += '\0';
    Check(TrampolinePage(nul).empty(), "a NUL");
  }
  {
    TEST_CASE("the page opens in the https browser first, then by its own association");
    const std::wstring chrome = L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
    // a profile with a space in its name: the path is one argument
    const std::wstring page =
        L"C:\\Users\\Ada Lovelace\\AppData\\Local\\Temp\\urnetwork-bridge-0123456789abcdef.html";
    const std::vector<TrampolineLaunch> launches = LaunchesFor(chrome, page);
    Check(launches.size() == 2, "two ways, in order");
    if (launches.size() == 2) {
      Check(launches[0] == TrampolineLaunch{chrome, L"\"" + page + L"\""},
            "first the https program, on the page's quoted path");
      Check(launches[1] == TrampolineLaunch{page, L""}, "then the page itself");
    }
  }
  {
    TEST_CASE("with no https program the page opens by its own association");
    const std::wstring page = L"C:\\Users\\ada\\AppData\\Local\\Temp\\urnetwork-bridge-0.html";
    const std::vector<TrampolineLaunch> launches = LaunchesFor(L"", page);
    Check(launches == std::vector<TrampolineLaunch>{{page, L""}}, "the page itself, alone");
  }
  if (argc > 1) {
    TEST_CASE("the payout consent's bridge urls go through the page");
    for (int i = 1; i < argc; ++i) {
      const std::string url = argv[i];
      Check(NeedsTrampoline(url), "url " + std::to_string(i) + " (" + std::to_string(url.size()) +
                                      " characters) is opened as it is");
      const std::string page = TrampolinePage(url);
      Check(page == PageFor(url), "url " + std::to_string(i) + " has no page of its own");
    }
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
