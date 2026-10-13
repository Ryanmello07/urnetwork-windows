// Executable spec for which network a browser approval is for and how a request names it
// (App/ApprovalNetwork.h): the grammar of `api` (contract section 11), class by class and
// against a second reading of it; the host a live API url names; the network a space is;
// and the contract version each network needs. Run against the same header the app
// compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App approval-network-tests.cpp -o /tmp/approval-network-tests && /tmp/approval-network-tests
//
// Every host is made up and obviously so (example.com and the like), but for api.beta-test.net,
// which is the contract's own conformance value.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "ApprovalNetwork.h"

using namespace urnw::approval;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;  // flushed: a spec that dies a moment later still says what failed
  }
}

void Accepts(std::string_view host, const std::string& what) {
  Check(IsValidApiHost(host), "api: " + what + " is a host (" + std::to_string(host.size()) + " characters)");
}

void Rejects(std::string_view host, const std::string& what) {
  Check(!IsValidApiHost(host), "api: " + what + " is no host (" + std::to_string(host.size()) + " characters)");
}

std::string Repeat(char c, std::size_t count) { return std::string(count, c); }

// ---- the grammar, by the contract's own examples ---------------------------------------------

void TestTheHostsTheContractAccepts() {
  Accepts("api.beta-test.net", "the conformance vector's api");
  Accepts("api.example.com:8443", "a host with a port (the contract's second example)");
  Accepts("example.com", "two labels");
  Accepts("a.b", "two labels of one character");
  Accepts("a1.b2", "labels with digits");
  Accepts("1a.2b", "labels that start with a digit");
  Accepts("a.1b", "a last label that has a digit but is not all digits");
  Accepts("a-b.c-d.example.org", "hyphens inside a label");
  Accepts("a--b.example", "two hyphens inside a label");
  Accepts("xn--bcher-kva.example", "a punycode label");
  Accepts("a.b.c.d.e.f.g.h", "many labels");
  Accepts("sub.api.beta-test.net:9", "a deep name and a one-digit port");
  // a number in any label but the last is a label like another, and so is a last label that only looks a little like one: a
  // browser reads a name as an address by its last label, and only when that is all digits or 0x and hex digits
  Accepts("0x7f.example.com", "a first label that is 0x and hex digits");
  Accepts("0x.example", "a first label of 0x");
  Accepts("1.2.3.example", "numbers in front of a last label that is a name");
  Accepts("a.0xg", "a last label that is 0x and a letter that is not a hex digit");
  Accepts("a.0x1g", "a last label that is 0x, a hex digit and a letter that is not one");
  Accepts("a.0x-1", "a last label that is 0x, a hyphen and a digit");
  Accepts("a.00x1", "a last label that is 00x1 (not 0x at its start)");
  Accepts("a.10x1", "a last label that is 10x1");
  Accepts("a.x0", "a last label that is x0");
  Accepts("a.x", "a last label of x");
  Accepts("a.0y", "a last label that is 0y");
  Check(!IsValidApiHost(""), "api: nothing is no host");
}

void TestEachRejectClassOfTheContract() {
  for (const char* host : {"API.example.com", "api.Example.com", "api.example.COM", "Api.beta-test.net", "A.B"}) {
    Rejects(host, std::string("upper case (") + host + ")");
  }
  for (const char* host : {"https://api.example.com", "http://api.example.com", "wss://api.example.com", "//api.example.com",
                           "https:api.example.com", "https://api.example.com:8443"}) {
    Rejects(host, std::string("a scheme (") + host + ")");
  }
  for (const char* host : {"api.example.com/", "api.example.com/v1", "api.example.com:8443/", "api.example.com/a/b"}) {
    Rejects(host, std::string("a path (") + host + ")");
  }
  for (const char* host : {"user@api.example.com", "user:pw@api.example.com", "@api.example.com", "api.example.com@evil.example"}) {
    Rejects(host, std::string("userinfo (") + host + ")");
  }
  for (const char* host : {"api.example.com?x=1", "api.example.com?", "api.example.com#frag", "api.example.com&x=1", "api.example.com=1"}) {
    Rejects(host, std::string("a query or a fragment (") + host + ")");
  }
  Rejects("api.example.com:443", "port 443");
  Rejects("a.b:443", "port 443 on the shortest host");
  Rejects("api.example.com:0", "port 0");
  Rejects("api.example.com:65536", "port 65536");
  Rejects("api.example.com:99999", "port 99999");
  Rejects("api.example.com:100000", "a six-digit port");
  Rejects("api.example.com:08080", "a leading-zero port");
  Rejects("api.example.com:00443", "443 behind two zeros");
  Rejects("api.example.com:0443", "443 behind a zero");
  Rejects("api.example.com:00", "port 00");
  Rejects("localhost", "a single label");
  Rejects("localhost:8080", "a single label with a port");
  Rejects("example", "another single label");
  Rejects("1", "a single digit");
  Rejects("1.2.3.4", "an IPv4 literal (the last label is all digits)");
  Rejects("10.0.0.1:8080", "an IPv4 literal with a port");
  Rejects("example.123", "an all-digit last label");
  Rejects("a.1", "the shortest all-digit last label");
  Rejects("a.0", "a last label of zero");
  // a browser reads a last label of 0x and hex digits as a number too: 1.2.3.0x4 is the address 1.2.3.4, and a name such as
  // a.0x10 is not a name at all, so the page could never switch to either
  Rejects("a.0x10", "a last label that is 0x and hex digits");
  Rejects("1.2.3.0x4", "an IPv4 address with a hex last part");
  Rejects("0x7f.0x0.0x0.0x1", "127.0.0.1 in hex");
  Rejects("a.0x", "a last label of 0x");
  Rejects("a.0x0", "a last label of 0x0");
  Rejects("q.0x1", "a last label of 0x1");
  Rejects("a.0xdeadbeef", "a long hex last label");
  Rejects("a.0xabcdef0123456789", "every hex digit in a last label");
  Rejects("example.com.0xff", "0xff at the end of a name that is otherwise fine");
  Rejects("a.0x10:8080", "a hex last label with a port");
  Rejects("a.b.c.d.e.0x1", "a hex last label after many labels");
  Rejects("[::1]", "an IPv6 literal");
  Rejects("[::1]:8080", "an IPv6 literal with a port");
  Rejects("[2001:db8::1]:8080", "a longer IPv6 literal with a port");
  Rejects("2001:db8::1", "a bare IPv6 address");
  Rejects("a..b", "an empty label in the middle");
  Rejects(".a.b", "a leading dot (an empty first label)");
  Rejects("a.b.", "a trailing dot");
  Rejects("example.com.", "a trailing dot after a name");
  Rejects("example.com.:8080", "a trailing dot before a port");
  Rejects("...", "dots only");
  Rejects(".", "one dot");
  Rejects("-a.b", "a label that starts with a hyphen");
  Rejects("a-.b", "a label that ends with a hyphen");
  Rejects("a.-b", "a last label that starts with a hyphen");
  Rejects("a.b-", "a last label that ends with a hyphen");
  Rejects("a.-.b", "a label of one hyphen");
  Rejects("-", "a hyphen");
  Rejects("--.--", "two labels of hyphens");
  Rejects(":8080", "a port and no host");
  Rejects("api.example.com:", "a colon and no port");
  Rejects("api.example.com::8080", "two colons");
  Rejects("api.example.com:80:80", "two ports");
  Rejects("api.example.com:80a", "a port with a letter");
  Rejects("api.example.com:a", "a port that is a letter");
  Rejects("api.example.com:-1", "a negative port");
  Rejects("api.example.com:+80", "a signed port");
  Rejects("api.example.com: 80", "a port with a blank before it");
  Rejects("api.example.com:80 ", "a port with a blank after it");
  Rejects("api.example.com:0x50", "a hexadecimal port");
  Rejects("api.example.com:8.0", "a port with a dot");
  Rejects("api.exa_mple.com", "an underscore");
  Rejects("api.exa mple.com", "a blank in a label");
  Rejects(" api.example.com", "a blank before the host");
  Rejects("api.example.com ", "a blank after the host");
  Rejects("api.example.com\n", "a line break after the host");
  Rejects("api.example\t.com", "a tab");
  Rejects(std::string("api.exam\0ple.com", 16), "a NUL byte");
  Rejects("api.exa%6Dple.com", "an escape");
  Rejects("api.exa+mple.com", "a plus");
  Rejects("api.exa*mple.com", "an asterisk");
  Rejects("api.exa,mple.com", "a comma");
  Rejects("api.exa;mple.com", "a semicolon");
  Rejects("api.exa\\mple.com", "a backslash");
  // outside ASCII: UTF-8 for u-umlaut, a fullwidth a, two Japanese characters, and a single byte over 0x7F
  Rejects("b\xC3\xBC" "cher.example", "a non-ASCII letter (UTF-8)");
  Rejects("\xEF\xBD\x81.example", "a fullwidth letter (UTF-8)");
  Rejects("\xE4\xBE\x8B\xE3\x81\x88.jp", "two Japanese characters (UTF-8)");
  Rejects("api.exam\x80ple.com", "a byte over 0x7F");
  Rejects("api.example.c\xFF" "m", "the byte 0xFF");
}

void TestEveryByteButTheHostAlphabetIsNoHostCharacter() {
  // one byte in the middle of a good name: only a-z 0-9 and the hyphen keep it a name (the dot is structure: it splits the
  // label, which is another case, below)
  for (int byte = 0; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    if (c == '.') continue;
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    std::string host = "api.exa" + std::string(1, c) + "mple.com";
    Check(IsValidApiHost(host) == allowed, "alphabet: byte " + std::to_string(byte) + " inside a label is " + (allowed ? "allowed" : "refused"));
  }
  // every letter and digit starts and ends a label; the hyphen does neither
  for (int byte = 0; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    const std::string first = std::string(1, c) + "pi.example.com";
    const std::string last = "api.example.co" + std::string(1, c);
    Check(IsValidApiHost(first) == alnum, "alphabet: byte " + std::to_string(byte) + " at the start of a label is " + (alnum ? "allowed" : "refused"));
    // the last label of a host is not all digits, so a digit at the end of 'co' still leaves letters in it
    Check(IsValidApiHost(last) == alnum, "alphabet: byte " + std::to_string(byte) + " at the end of a label is " + (alnum ? "allowed" : "refused"));
  }
}

void TestTheLimits() {
  Check(kMaxApiChars == 200 && kMaxApiLabelChars == 63 && kMaxApiPort == 65535 && kHttpsPort == 443,
        "constants: 200 characters, labels of 63, ports up to 65535 and never 443");
  // a label of 63 characters is the longest
  const std::string longest = Repeat('a', 63);
  Accepts(longest + ".example", "a first label of 63 characters");
  Accepts("example." + longest, "a last label of 63 characters");
  Rejects(Repeat('a', 64) + ".example", "a first label of 64 characters");
  Rejects("example." + Repeat('a', 64), "a last label of 64 characters");
  Accepts("a" + Repeat('-', 61) + "a.example", "a label of 63 characters that is hyphens inside");
  Rejects("a" + Repeat('-', 62) + "a.example", "the same with 64");
  // 200 characters in all, and one more
  const std::string name195 = longest + "." + longest + "." + longest + ".bcd";
  Check(name195.size() == 195, "limit: the name used below is 195 characters");
  const std::string name200 = longest + "." + longest + "." + longest + ".bcdefghi";
  Check(name200.size() == 200, "limit: and this one is 200");
  Accepts(name200, "a host of exactly 200 characters");
  Rejects(name200 + "i", "a host of 201 characters");
  Accepts(name195 + ":8080", "a host and a port that make exactly 200 characters");
  Rejects(name195 + "e:8080", "the same with one character more (the port counts)");
  Accepts(name195 + ":80", "a shorter port in the same room");
  Rejects(Repeat('a', 300), "a single label of 300 characters");
  Rejects("a." + Repeat('b', 20000), "20,000 characters");
}

void TestThePorts() {
  Accepts("a.b:1", "port 1");
  Accepts("a.b:80", "port 80");
  Accepts("a.b:442", "port 442");
  Accepts("a.b:444", "port 444");
  Accepts("a.b:4430", "port 4430 (443 is only 443)");
  Accepts("a.b:8443", "port 8443");
  Accepts("a.b:65535", "port 65535");
  Accepts("a.b:65534", "port 65534");
  Accepts("a.b:10000", "port 10000");
  Rejects("a.b:65536", "port 65536");
  Rejects("a.b:65540", "port 65540");
  Rejects("a.b:70000", "port 70000");
  Rejects("a.b:0", "port 0");
  Rejects("a.b:443", "port 443");
  Rejects("a.b:01", "port 01");
  Rejects("a.b:000001", "port 000001");
  // every one-, two- and three-digit port is a port, but for a leading zero, and 443
  for (int port = 0; port <= 1000; ++port) {
    const std::string text = std::to_string(port);
    Check(IsValidApiHost("a.b:" + text) == (port >= 1 && port != 443), "port: " + text);
    if (port >= 1) {
      Check(!IsValidApiHost("a.b:0" + text), "port: 0" + text + " has a leading zero");
    }
  }
  // the edges of the range, five digits
  for (int port : {9999, 10000, 22222, 44444, 55555, 65534, 65535, 65536, 65537, 66666, 99999}) {
    Check(IsValidApiHost("a.b:" + std::to_string(port)) == (port <= 65535), "port: " + std::to_string(port));
  }
  // a byte that is not a digit is none between two digits, whatever its value would add up to (a test of the digits that took a
  // neighbour of '9' would read the colon as ten, and 1:1 as 201)
  for (int byte = 0; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    if (c >= '0' && c <= '9') continue;
    Check(!IsValidApiHost("a.b:1" + std::string(1, c) + "1"), "port: byte " + std::to_string(byte) + " between two digits");
  }
  // a port too long to be one is refused before its digits are added up: these wrap around 32 bits to 445 and to 0
  Rejects("a.b:4294967741", "a ten-digit port that wraps around to 445");
  Rejects("a.b:4294967296", "a ten-digit port that wraps around to 0");
  Rejects("a.b:" + Repeat('1', 20), "a port of twenty digits");
  Rejects("a.b:" + Repeat('9', 25), "a port of twenty-five digits");
}

// A browser decides by the last label of a name whether it is an address: the URL standard's "ends in a number" check takes a
// last label that is all digits, or 0x and any hex digits (0x alone is the number 0), and then the name has to be an IPv4
// address, which it very seldom is. So every such last label is no host here, wherever its number would lead, and the near
// misses (a letter that is no hex digit, a number that does not start at the first character) are hosts.
void TestTheLastLabelDecidesWhetherANameIsANumber() {
  const std::string hex = "0123456789abcdef";
  std::vector<std::string> digits = {""};
  std::size_t numbers = 0;
  for (std::size_t length = 0; length <= 3; ++length) {
    std::vector<std::string> next;
    for (const std::string& d : digits) {
      ++numbers;
      Check(!IsValidApiHost("a.0x" + d), "last label: 0x" + d + " is a number, so no host");
      Check(!IsValidApiHost("1.2.3.0x" + d + ":8080"), "last label: 0x" + d + " is a number with a port as well");
      Check(IsValidApiHost("0x" + d + ".example"), "first label: 0x" + d + " is a label like another, so a host");
      if (!d.empty()) {
        std::string nearMiss = d;
        nearMiss.back() = 'g';
        Check(IsValidApiHost("a.0x" + nearMiss), "last label: 0x" + nearMiss + " has a letter that is no hex digit, so a host");
        Check(IsValidApiHost("a.0" + d + "x"), "last label: 0" + d + "x does not start with 0x, so a host");
        // a letter among the digits makes 0<digits> a name, and a zero that is not followed by an x makes it no hex number
        if (d.find_first_of("abcdef") != std::string::npos) {
          Check(IsValidApiHost("a.0" + d), "last label: 0" + d + " is a name (a letter, and no 0x), so a host");
        }
      }
      if (length < 3) {
        for (const char c : hex) next.push_back(d + c);
      }
    }
    digits = std::move(next);
  }
  Check(numbers == 1 + 16 + 256 + 4096, "last label: every 0x with up to three hex digits was checked (" + std::to_string(numbers) + ")");
  Rejects("a.0xA", "a hex last label in capitals (upper case is refused anyway)");
  Rejects("a.0X1", "0X1 (upper case is refused anyway)");
}

// The helper under that rule, asked directly: a label reaches it only after the label rule has taken it, but a helper that is right
// only for what reaches it is one that a later change breaks. It reads 0, x and then hex digits, byte by byte, and never past the end
// of the view it is given.
void TestTheHexNumberHelperReadsOnlyWhatItIsGiven() {
  Check(!detail::IsHexNumber(""), "hex number: nothing is not one");
  Check(!detail::IsHexNumber("0"), "hex number: 0 is not one");
  Check(detail::IsHexNumber("0x"), "hex number: 0x is one");
  Check(!detail::IsHexNumber(std::string_view("0x", 1)), "hex number: a view of the 0 in front of an x is not one, and does not read the x");
  for (int byte = 0; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    const std::string text = "0x1" + std::string(1, c);
    Check(detail::IsHexNumber(text) == hex, "hex number: byte " + std::to_string(byte) + " after 0x1 is " + (hex ? "a hex digit" : "no hex digit"));
    Check(detail::IsHexNumber("0x" + std::string(1, c)) == hex, "hex number: byte " + std::to_string(byte) + " after 0x is " + (hex ? "a hex digit" : "no hex digit"));
    Check(detail::IsHexNumber(std::string(1, c) + "x1") == (c == '0'), "hex number: byte " + std::to_string(byte) + " in front of x1 is " + (c == '0' ? "the 0" : "not a 0"));
    Check(detail::IsHexNumber("0" + std::string(1, c) + "1") == (c == 'x'), "hex number: byte " + std::to_string(byte) + " between 0 and 1 is " + (c == 'x' ? "the x" : "not an x"));
  }
}

// ---- a second reading of the grammar -------------------------------------------------------------

// The contract's sentence, read again as a regular expression and three more checks, with nothing shared with the header.
bool Oracle(const std::string& text) {
  static const std::regex shape(
      R"(([a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(:[1-9][0-9]{0,4})?)");
  static const std::regex hexNumber("0x[0-9a-f]*");
  if (text.size() > 200 || !std::regex_match(text, shape)) return false;
  const std::size_t colon = text.find(':');
  const std::string name = text.substr(0, colon);
  const std::string last = name.substr(name.rfind('.') + 1);
  if (last.find_first_not_of("0123456789") == std::string::npos) return false;
  if (std::regex_match(last, hexNumber)) return false;
  if (colon != std::string::npos) {
    const long port = std::stol(text.substr(colon + 1));
    if (port > 65535 || port == 443) return false;
  }
  return true;
}

void TestTheGrammarAgreesWithASecondReadingOfIt() {
  // every string of up to five characters over the characters that make the grammar interesting (x is what 0x needs)
  const std::string alphabet = "a1-.:043x";
  std::size_t accepted = 0;
  std::size_t total = 0;
  std::vector<std::string> level{""};
  for (int length = 0; length <= 5; ++length) {
    std::vector<std::string> next;
    for (const std::string& text : level) {
      ++total;
      const bool want = Oracle(text);
      if (want) ++accepted;
      if (IsValidApiHost(text) != want) Check(false, "oracle: '" + text + "' is " + (want ? "a host" : "no host"));
      if (length < 5) {
        for (const char c : alphabet) next.push_back(text + c);
      }
    }
    level = std::move(next);
  }
  Check(total == 66430, "oracle: every string of up to five characters was compared (" + std::to_string(total) + ")");
  Check(accepted > 100, "oracle: and a good many of them are hosts (" + std::to_string(accepted) + ")");

  // hosts built from the pieces a real one has: labels that are good, bad at an edge, all digits, 0x and hex digits, 63 or 64
  // long, and the ends a host can have (a trailing dot, ports at and past their limits, 443)
  const std::vector<std::string> labels = {"",     "a",    "1",     "-",     "a-",    "-a",         "a-a",     "a1",
                                           "1a",   "11",   "A",     "a_",    "xn--a", Repeat('a', 63), Repeat('b', 64),
                                           "0x",   "0x1f", "0xg",   "00x1",  "x0"};
  const std::vector<std::string> ends = {"",       ".",      ":",     ":0",    ":1",     ":80",   ":443",    ":4430",
                                         ":8443",  ":65535", ":65536", ":99999", ":100000", ":080", ":a",      ":8:8",
                                         ":-1",    ":00",    ":0443"};
  std::size_t built = 0;
  std::size_t builtHosts = 0;
  for (const std::string& first : labels) {
    for (const std::string& second : labels) {
      for (const std::string& end : ends) {
        for (const std::string& third : {std::string(), std::string("."), std::string(".a"), std::string(".1"), std::string(".b-"),
                                         std::string(".0x"), std::string(".0x9")}) {
          const std::string text = first + "." + second + third + end;
          const bool want = Oracle(text);
          ++built;
          if (want) ++builtHosts;
          if (IsValidApiHost(text) != want) Check(false, "oracle: '" + text + "' is " + (want ? "a host" : "no host"));
        }
      }
    }
  }
  Check(built == labels.size() * labels.size() * ends.size() * 7, "oracle: the built hosts were all compared");
  Check(builtHosts > 100, "oracle: and a good many of them are hosts (" + std::to_string(builtHosts) + ")");
}

// ---- the host an API url names -----------------------------------------------------------------

void TestAnApiUrlNamesItsHostWhenItIsExactlyHttpsAndAHost() {
  Check(ApiHostOfUrl("https://api.beta-test.net") == "api.beta-test.net", "url: the vector's api url names its host");
  Check(ApiHostOfUrl("https://api.beta-test.net/") == "api.beta-test.net", "url: one trailing slash is allowed");
  Check(ApiHostOfUrl("https://api.example.com:8443") == "api.example.com:8443", "url: a port is part of the name");
  Check(ApiHostOfUrl("https://api.example.com:8443/") == "api.example.com:8443", "url: a port and one trailing slash");
  Check(ApiHostOfUrl("https://a.b") == "a.b", "url: the shortest host");
  Check(ApiHostOfUrl("https://api.example.com:1") == "api.example.com:1", "url: port 1");
  Check(ApiHostOfUrl("https://api.example.com:65535") == "api.example.com:65535", "url: port 65535");
  Check(ApiHostOfUrl("https://" + Repeat('a', 63) + ".example") == Repeat('a', 63) + ".example", "url: a label of 63 characters");

  // A URL is read the way a URL is read: its scheme and its host are the same in capitals, and https' own port is the port
  // it does not spell. The client dials the same server either way, so the value is in small letters and has no ":443".
  const auto names = [](std::string_view url, std::string_view api, const std::string& what) {
    Check(ApiHostOfUrl(url) == api, "url: " + what + " names " + std::string(api));
  };
  names("HTTPS://api.example.com", "api.example.com", "a scheme in capitals");
  names("Https://api.example.com", "api.example.com", "a scheme in mixed case");
  names("https://API.EXAMPLE.COM", "api.example.com", "an upper-case host");
  names("https://api.Example.com", "api.example.com", "a host with a capital");
  names("HTTPS://API.Beta-Test.NET/", "api.beta-test.net", "capitals everywhere, and a trailing slash");
  names("https://api.example.com:443", "api.example.com", "port 443 spelled out");
  names("https://api.example.com:443/", "api.example.com", "port 443 spelled out, with a slash");
  names("https://API.Example.com:443", "api.example.com", "port 443 spelled out and capitals");
  names("https://a.b:443", "a.b", "port 443 on the shortest host");
  names("https://api.example.com:44", "api.example.com:44", "port 44, which is not 443");
  names("https://api.example.com:4430", "api.example.com:4430", "port 4430, which is not 443");
  names("https://api.example.com:1443", "api.example.com:1443", "port 1443, which is not 443");
  names("https://api.example.com:8443", "api.example.com:8443", "port 8443");
  // every capital is folded to its own small letter, and only the capitals are: the alphabet one letter at a time
  for (char upper = 'A'; upper <= 'Z'; ++upper) {
    const char lower = static_cast<char>(upper - 'A' + 'a');
    names("https://" + std::string(1, upper) + ".example", std::string(1, lower) + ".example", std::string("a host that starts with ") + upper);
    names("HTTPS://x." + std::string(1, upper) + std::string(1, upper), std::string("x.") + lower + lower, std::string("a last label of ") + upper + upper);
  }
  names("https://API.Example.com:8443/", "api.example.com:8443", "port 8443 in capitals, with a slash");

  const auto nothing = [](std::string_view url, const std::string& what) {
    Check(ApiHostOfUrl(url).empty(), "url: " + what + " names nothing");
  };
  nothing("", "an empty url");
  nothing("https://", "a scheme and no host");
  nothing("https:///", "a scheme, a slash and no host");
  nothing("https://api.example.com//", "two trailing slashes");
  nothing("https://api.example.com/v1", "a path");
  nothing("https://api.example.com/v1/", "a path and a trailing slash");
  nothing("https://api.example.com/?x=1", "a slash and a query");
  nothing("https://api.example.com?x=1", "a query");
  nothing("https://api.example.com#frag", "a fragment");
  nothing("https://api.example.com/#frag", "a slash and a fragment");
  nothing("https://user@api.example.com", "a user");
  nothing("https://user:pw@api.example.com", "a user and a password");
  nothing("http://api.example.com", "an http api");
  nothing("http://api.example.com:8080", "an http api with a port");
  nothing("HTTP://api.example.com", "an http api in capitals");
  nothing("wss://api.example.com", "another scheme");
  nothing("ftp://api.example.com", "ftp");
  nothing("api.example.com", "no scheme");
  nothing("//api.example.com", "a scheme-relative url");
  nothing("https:/api.example.com", "one slash after the scheme");
  nothing("https:api.example.com", "no slash after the scheme");
  nothing("https://https://api.example.com", "a scheme twice");
  nothing("https://api.example.com:443//", "port 443 spelled out, with two slashes");
  nothing("https://api.example.com::443", "a colon in front of port 443");
  nothing("https://api.example.com:443:443", "port 443 twice");
  nothing("https://api.example.com:8443:443", "port 443 after another port");
  nothing("https://a.b:1:443", "port 443 after port 1");
  nothing("https://api.example.com:443:8443", "port 443 before another port");
  nothing("https://api.example.com:0443", "443 behind a zero");
  nothing("https://:443", "port 443 and no host");
  nothing("https://1.2.3.0x4", "an IPv4 address with a hex last part (a browser reads it as 1.2.3.4)");
  nothing("https://1.2.3.0x4/", "the same with a trailing slash");
  nothing("https://a.0x10", "a last label that is 0x and hex digits (a browser refuses it)");
  nothing("https://0x7f.0x0.0x0.0x1:8443", "127.0.0.1 in hex, with a port");
  nothing("https://api.exam%70le.com", "an escape");
  nothing("https://\xE2\x84\xAA.example", "a Kelvin sign, which a browser folds to a small k");
  nothing("https://\xC3\x84PI.example", "a capital letter outside ASCII");
  nothing("https://api.example.com:0", "port 0");
  nothing("https://api.example.com:65536", "port 65536");
  nothing("https://api.example.com:08080", "a leading-zero port");
  nothing("https://api.example.com:", "a colon and no port");
  nothing("https://127.0.0.1", "an IPv4 literal");
  nothing("https://127.0.0.1:8443", "an IPv4 literal with a port");
  nothing("https://[::1]", "an IPv6 literal");
  nothing("https://[::1]:8443", "an IPv6 literal with a port");
  nothing("https://localhost", "a single label");
  nothing("https://localhost:8443", "a single label with a port");
  nothing("https://b\xC3\xBC" "cher.example", "a host outside ASCII");
  nothing("https://api.example.com.", "a trailing dot");
  nothing("https://api.example.com. ", "a blank at the end");
  nothing("https://api.example.com\n", "a line break at the end");
  nothing("https://api.exa mple.com", "a blank inside");
  nothing("https://api.example.com/ ", "a slash and a blank");
  nothing("https://" + Repeat('a', 64) + ".example", "a label of 64 characters");
  // the longest host that fits is named
  const std::string name200 = Repeat('a', 63) + "." + Repeat('a', 63) + "." + Repeat('a', 63) + ".bcdefghi";
  Check(name200.size() == 200, "url: the longest host is 200 characters");
  Check(ApiHostOfUrl("https://" + name200) == name200, "url: a host of 200 characters is named");
  Check(ApiHostOfUrl("https://" + name200 + "/") == name200, "url: and with its trailing slash");
  nothing("https://" + name200 + "i", "a host of 201 characters");
}

// ---- the network a space is --------------------------------------------------------------------------

void TestTheNetworkOfASpace() {
  const Network none;
  Check(none.kind == NetworkKind::None && none.api.empty(), "network: a network that was never read is none, and names nothing");

  // the official network never names itself: the operator's space, whose API is the operator's own api.<host>, in whatever way
  // that url is spelled (a trailing slash, capitals, https' own port)
  for (const char* url : {"https://api.bringyour.com", "https://api.bringyour.com/", "https://API.Bringyour.com", "HTTPS://api.bringyour.com",
                          "https://api.bringyour.com:443"}) {
    const Network official = NetworkOf(true, url, "bringyour.com");
    Check(official.kind == NetworkKind::Official && official.api.empty(), std::string("network: the official space is official and names nothing (url '") + url + "')");
  }

  // ... and only that: the operator's host with another API under it is not the network the page's default server serves. A
  // test environment of the operator has its name in front of the service label, and a request for the official network would
  // be minted on the default server and redeemed on that one, so it is named like any other space
  for (const char* url : {"https://dev-api.bringyour.com", "https://main-api.bringyour.com", "https://api.bringyour.com:8443",
                          "https://api.beta-test.net", "https://sub.api.bringyour.com", "https://api.bringyour.com.example"}) {
    const Network lookalike = NetworkOf(true, url, "bringyour.com");
    Check(lookalike.kind == NetworkKind::Custom && lookalike.api == ApiHostOfUrl(url) && !lookalike.api.empty(),
          std::string("network: the operator's space whose api url is '") + url + "' is custom, and names that api");
  }
  const Network dev = NetworkOf(true, "https://dev-api.bringyour.com", "bringyour.com");
  Check(dev.kind == NetworkKind::Custom && dev.api == "dev-api.bringyour.com", "network: a test environment of the operator names its own API");
  // the operator's host is a parameter, not a name this header knows
  const Network other = NetworkOf(true, "https://api.example.org", "example.org");
  Check(other.kind == NetworkKind::Official && other.api.empty(), "network: another operator's own api is the official one for it");
  const Network notOther = NetworkOf(true, "https://api.bringyour.com", "example.org");
  Check(notOther.kind == NetworkKind::Custom && notOther.api == "api.bringyour.com", "network: and not for an operator whose api it is not");
  const Network noOperator = NetworkOf(true, "https://api.bringyour.com", "");
  Check(noOperator.kind == NetworkKind::Custom && noOperator.api == "api.bringyour.com", "network: with no operator there is no official network");

  // ... and a space whose url cannot be read is nothing at all, the operator's included: the app cannot say which server it talks to
  for (const char* url : {"", "http://api.bringyour.com", "https://api.bringyour.com/v1", "https://api.bringyour.com/secret", "garbage",
                          "https://127.0.0.1", "https://api.bringyour.com?x"}) {
    const Network unread = NetworkOf(true, url, "bringyour.com");
    Check(unread.kind == NetworkKind::None && unread.api.empty(), std::string("network: the operator's space whose api url is '") + url + "' names nothing");
  }

  // any other space is named by its live API url
  const Network beta = NetworkOf(false, "https://api.beta-test.net", "bringyour.com");
  Check(beta.kind == NetworkKind::Custom && beta.api == "api.beta-test.net", "network: another space is custom and names its API host");
  const Network port = NetworkOf(false, "https://api.example.com:8443/", "bringyour.com");
  Check(port.kind == NetworkKind::Custom && port.api == "api.example.com:8443", "network: with its port, and without the trailing slash");
  const Network typed = NetworkOf(false, "https://API.Beta-Test.net:443", "bringyour.com");
  Check(typed.kind == NetworkKind::Custom && typed.api == "api.beta-test.net", "network: as typed in capitals and with https' own port, it is the same host");

  // ... unless the url cannot be named, and then it names nothing and the action is not offered
  for (const char* url : {"", "http://api.example.com", "https://127.0.0.1", "https://api.example.com/v1", "https://api.example.com:0",
                          "https://user@api.example.com", "https://localhost", "api.example.com", "https://1.2.3.0x4",
                          "https://api.example.com?x", "https://b\xC3\xBC" "cher.example"}) {
    const Network unnamed = NetworkOf(false, url, "bringyour.com");
    Check(unnamed.kind == NetworkKind::None && unnamed.api.empty(), std::string("network: a space whose api url is '") + url + "' names nothing");
  }

  // the API url the client really uses is what counts, so a space that is not the operator's but whose url is the operator's
  // own (an override typed back in) is custom and names the operator's host, which the page then finds is its default
  const Network typedBack = NetworkOf(false, "https://api.bringyour.com", "bringyour.com");
  Check(typedBack.kind == NetworkKind::Custom && typedBack.api == "api.bringyour.com",
        "network: a space that is not the operator's but uses the operator's api names it");

  Check(Network{} == none && beta == NetworkOf(false, "https://api.beta-test.net", "bringyour.com") && !(beta == port) &&
            !(Network{NetworkKind::Official, ""} == none) && !(Network{NetworkKind::Custom, "a.b"} == Network{NetworkKind::Custom, "a.c"}),
        "network: two networks are the same if the kind and the name are");
}

void TestEachNetworkNeedsItsOwnContract() {
  Check(kContract == 1 && kContractNamed == 2, "contract: the versions this app speaks are 1 and 2");
  Check(ContractFor(NetworkKind::Official) == 1, "contract: the official network is version 1");
  Check(ContractFor(NetworkKind::Custom) == 2, "contract: another network is version 2");
  Check(ContractFor(NetworkKind::None) == 0, "contract: no network is no version");
  Check(ContractFor(NetworkKind::Official) != ContractFor(NetworkKind::Custom), "contract: and they differ");
}

}  // namespace

int main() {
  TestTheHostsTheContractAccepts();
  TestEachRejectClassOfTheContract();
  TestEveryByteButTheHostAlphabetIsNoHostCharacter();
  TestTheLimits();
  TestThePorts();
  TestTheLastLabelDecidesWhetherANameIsANumber();
  TestTheHexNumberHelperReadsOnlyWhatItIsGiven();
  TestTheGrammarAgreesWithASecondReadingOfIt();
  TestAnApiUrlNamesItsHostWhenItIsExactlyHttpsAndAHost();
  TestTheNetworkOfASpace();
  TestEachNetworkNeedsItsOwnContract();
  std::cout << (gCases - gFailures) << "/" << gCases << " approval network checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
