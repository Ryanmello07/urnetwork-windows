// Executable spec for the urnetwork://auth return (App/AuthLink.h): which links
// the strict parser accepts (the page's own among them, and every character of
// the alphabets its values are made of), why it refuses every other (a percent
// sign anywhere among them), which links route to the auth gate, where a command
// line holds a link, and what the log gets instead of one. Run against the same
// header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App auth-link-tests.cpp -o /tmp/auth-link-tests && /tmp/auth-link-tests
//
// Every value below is made up and obviously so (43 'S' for a state, 44 'E' for a
// public key); none is a real code, key or state. The sweeps over the alphabets
// rotate through the legal characters, and the page's conformance vector (see
// kPageEpk) is a sealed answer made with fixed test keys that are not secret.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdio>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "AuthLink.h"
#include "approval-vector.h"

using namespace urnw;
using authlink::Kind;
using authlink::Reject;

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

// 43 base64url characters, as the page makes from 32 random bytes.
const std::string kState(43, 'S');
// base58 of a 32-byte public key (44 characters), of a 24-byte nonce (33) and of
// the sealed box around a 684-character code (1,036), as the page's vector has them.
const std::string kEpk(44, 'E');
const std::string kNonce(33, 'N');
const std::string kData(1036, 'D');

const std::string kAuth = "urnetwork://auth";

std::string Sealed(bool slash = false) {
  return kAuth + (slash ? "/" : "") + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData;
}

std::string Denied(bool slash = false) {
  return kAuth + (slash ? "/" : "") + "?state=" + kState + "&error=access_denied";
}

std::string Reason(const authlink::Return& r) { return std::string(authlink::ToString(r.reject)); }

void ExpectReject(const std::string& url, Reject why, const std::string& what,
                  const authlink::Contract& contract = authlink::kApprovalV1) {
  const authlink::Return r = authlink::Parse(url, contract);
  Check(r.kind == Kind::Invalid && r.reject == why, what + " (got " + Reason(r) + ")");
  Check(r.state.empty() && r.epk.empty() && r.nonce.empty() && r.data.empty(),
        what + ": a refused link carries no value");
}

void TestTheParserAcceptsBothShapes() {
  authlink::Return r = authlink::Parse(Sealed());
  Check(r.kind == Kind::Sealed && r.reject == Reject::None && r.state == kState && r.epk == kEpk &&
            r.nonce == kNonce && r.data == kData,
        "sealed: the page's four values");
  Check(Sealed().size() < 2000, "sealed: the page's own cap on the whole address (2,000) holds for these lengths");
  r = authlink::Parse(Sealed(/*slash=*/true));
  Check(r.kind == Kind::Sealed && r.data == kData, "sealed: with the browser's '/' after the host");
  r = authlink::Parse(kAuth + "?d=" + kData + "&n=" + kNonce + "&epk=" + kEpk + "&state=" + kState);
  Check(r.kind == Kind::Sealed && r.state == kState && r.epk == kEpk && r.nonce == kNonce && r.data == kData,
        "sealed: the order of the keys does not matter");
  r = authlink::Parse(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + std::string(2048, 'D'));
  Check(r.kind == Kind::Sealed && r.data.size() == 2048, "sealed: a value of exactly the cap");

  r = authlink::Parse(Denied());
  Check(r.kind == Kind::Denied && r.reject == Reject::None && r.state == kState, "denied: the one error value");
  Check(r.epk.empty() && r.nonce.empty() && r.data.empty(), "denied: nothing sealed, and the error value is not kept");
  r = authlink::Parse(Denied(/*slash=*/true));
  Check(r.kind == Kind::Denied, "denied: with the browser's '/' after the host");
  r = authlink::Parse(kAuth + "?error=access_denied&state=" + kState);
  Check(r.kind == Kind::Denied && r.state == kState, "denied: the order of the keys does not matter");
}

void TestTheParserRefusesEverythingElse() {
  // the host
  ExpectReject("urnetwork://authenticator?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "the host goes on: authenticator");
  ExpectReject("urnetwork://auth.evil.example?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "the host goes on: auth.evil.example");
  ExpectReject("URNETWORK://auth?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "the scheme is lowercase only: Parse is strict where routing is not");
  ExpectReject("urnetwork://AUTH?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "the host is lowercase only: Parse is strict where routing is not");
  ExpectReject("urnetwork://Auth/?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "a host in mixed case is not the host, with or without the '/'");
  ExpectReject("urnetwork://oauth/google?state=" + kState + "&id_token=x", Reject::NotAuthHost, "another host");
  ExpectReject("https://ur.io/?state=" + kState + "&error=access_denied", Reject::NotAuthHost, "another scheme");
  ExpectReject("", Reject::NotAuthHost, "nothing at all");
  ExpectReject("urnetwork://", Reject::NotAuthHost, "no host");
  ExpectReject("urnetwork://auth@evil/?state=" + kState + "&error=access_denied", Reject::Authority, "userinfo");
  ExpectReject("urnetwork://auth:80/?state=" + kState + "&error=access_denied", Reject::Authority, "a port");

  // what may follow the host
  ExpectReject("urnetwork://auth/x?state=" + kState + "&error=access_denied", Reject::Path, "a path");
  ExpectReject("urnetwork://auth/x/?state=" + kState + "&error=access_denied", Reject::Path, "a longer path");
  ExpectReject("urnetwork://auth//?state=" + kState + "&error=access_denied", Reject::Path,
               "two slashes: one is tolerated, not two");
  ExpectReject(Denied() + "#fragment", Reject::Delimiter, "a fragment after the query");
  ExpectReject(Sealed() + "#fragment", Reject::Delimiter, "a fragment after a sealed query");
  ExpectReject("urnetwork://auth#state=" + kState + "&error=access_denied", Reject::Delimiter,
               "a fragment where the query belongs");
  ExpectReject("urnetwork://auth/#state=" + kState + "&error=access_denied", Reject::Delimiter,
               "a fragment after the tolerated '/'");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied?x", Reject::Delimiter, "a second '?'");
  ExpectReject("urnetwork://auth", Reject::EmptyPair, "no pairs at all");
  ExpectReject("urnetwork://auth/", Reject::EmptyPair, "no pairs after the tolerated '/'");
  ExpectReject("urnetwork://auth?", Reject::EmptyPair, "an empty query");

  // the pairs
  ExpectReject("urnetwork://auth?state=" + kState + "&state=" + kState + "&error=access_denied", Reject::Duplicate,
               "a key twice, even with the same value");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied&error=access_denied", Reject::Duplicate,
               "the error twice");
  ExpectReject(Sealed() + "&d=" + kData, Reject::Duplicate, "the box twice");
  ExpectReject(Sealed() + "&a=1&b=2&c=3", Reject::TooLong, "a seventh pair is over the cap before its key is read");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&n=" + kNonce, Reject::Duplicate,
               "the nonce twice");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied&x=1", Reject::UnknownKey, "an unknown key");
  ExpectReject("urnetwork://auth?State=" + kState + "&error=access_denied", Reject::UnknownKey, "keys are lowercase");
  ExpectReject("urnetwork://auth?state=" + kState + "&&error=access_denied", Reject::EmptyPair, "an empty segment");
  ExpectReject("urnetwork://auth?&state=" + kState + "&error=access_denied", Reject::EmptyPair, "a leading '&'");
  ExpectReject(Denied() + "&", Reject::EmptyPair, "a trailing '&'");
  ExpectReject("urnetwork://auth?state=" + kState + "&error", Reject::EmptyPair, "a pair without '='");
  ExpectReject("urnetwork://auth?state=" + kState + "&=abc", Reject::EmptyPair, "a pair without a key");

  // the plain code is not an answer the page sends, so nothing reads one
  ExpectReject("urnetwork://auth?state=" + kState + "&code=" + std::string(683, 'C') + "=", Reject::UnknownKey,
               "a code in the clear beside the state");
  ExpectReject("urnetwork://auth?code=" + std::string(683, 'C') + "=&state=" + kState, Reject::UnknownKey,
               "a code in the clear, whatever the order");
  ExpectReject("urnetwork://auth/?state=" + kState + "&code=abc", Reject::UnknownKey,
               "a code in the clear, with the browser's '/'");
  ExpectReject(Sealed() + "&code=abc", Reject::UnknownKey, "a code beside a sealed answer");
  ExpectReject("urnetwork://auth?code=abc", Reject::UnknownKey, "a code alone");
  ExpectReject("urnetwork://auth?auth_code=abc&state=" + kState, Reject::UnknownKey, "the code under its api name");

  // bytes
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access denied", Reject::Control, "a raw space");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access\tdenied", Reject::Control, "a raw tab");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access\ndenied", Reject::Control, "a raw line feed");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access\rdenied", Reject::Control, "a raw carriage return");
  ExpectReject(std::string("urnetwork://auth?state=") + kState + std::string("&error=ac\0ess_denied", 20),
               Reject::Control, "an embedded NUL");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied\x7f", Reject::Control, "a raw DEL");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denie\xC3\xA9", Reject::Control,
               "a raw non-ASCII byte");

  // sizes
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + std::string(5000, 'D'),
               Reject::TooLong, "a url over the cap");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + std::string(2049, 'D'),
               Reject::TooLong, "a value one over the cap");
  ExpectReject("urnetwork://auth?state=" + kState + "&epk=a&n=b&d=c&x=1&y=2&z=3", Reject::TooLong,
               "more than six pairs");

  // the state
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + "&error=access_denied", Reject::BadState,
               "a 42-character state");
  ExpectReject("urnetwork://auth?state=" + kState + "S&error=access_denied", Reject::BadState, "a 44-character state");
  ExpectReject("urnetwork://auth?state=&error=access_denied", Reject::BadState, "an empty state");
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + "+&error=access_denied", Reject::BadState,
               "'+' is not base64url");
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + "=&error=access_denied", Reject::BadState,
               "padding in a state");
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + "/&error=access_denied", Reject::BadState,
               "standard base64's '/' is not base64url");
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + ".&error=access_denied", Reject::BadState,
               "'.' is not base64url");
  ExpectReject("urnetwork://auth?state=" + kState.substr(1) + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData,
               Reject::BadState, "a 42-character state in a sealed answer");

  // the sealed values
  ExpectReject(kAuth + "?state=" + kState + "&epk=0OIl&n=" + kNonce + "&d=" + kData, Reject::BadValue,
               "base58 has no 0, O, I or l");
  ExpectReject(kAuth + "?state=" + kState + "&epk=&n=" + kNonce + "&d=" + kData, Reject::BadValue, "an empty epk");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=&d=" + kData, Reject::BadValue, "an empty nonce");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=", Reject::BadValue, "an empty box");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData + "+", Reject::BadValue,
               "'+' in the sealed data");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData + "=", Reject::BadValue,
               "padding in the sealed data");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + std::string(65, 'E') + "&n=" + kNonce + "&d=" + kData,
               Reject::BadValue, "an epk longer than a key can be");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + std::string(65, 'N') + "&d=" + kData,
               Reject::BadValue, "a nonce longer than a nonce can be");

  // the shapes
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied&epk=" + kEpk, Reject::MixedShapes,
               "an error beside an epk");
  ExpectReject(Sealed() + "&error=access_denied", Reject::MixedShapes, "an error beside a whole sealed answer");
  ExpectReject("urnetwork://auth?error=access_denied", Reject::MixedShapes, "no state in a denial");
  ExpectReject("urnetwork://auth?epk=" + kEpk + "&n=" + kNonce + "&d=" + kData, Reject::MixedShapes,
               "no state in a sealed answer");
  ExpectReject("urnetwork://auth?state=" + kState, Reject::MixedShapes, "a state alone");
  ExpectReject("urnetwork://auth?state=" + kState + "&epk=" + kEpk, Reject::MixedShapes, "an epk without n and d");
  ExpectReject("urnetwork://auth?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce, Reject::MixedShapes,
               "an epk and n without d");
  ExpectReject("urnetwork://auth?state=" + kState + "&n=" + kNonce + "&d=" + kData, Reject::MixedShapes,
               "n and d without an epk");
  ExpectReject("urnetwork://auth?state=" + kState + "&epk=" + kEpk + "&d=" + kData, Reject::MixedShapes,
               "an epk and d without n");

  // the error value
  ExpectReject("urnetwork://auth?state=" + kState + "&error=ACCESS_DENIED", Reject::BadError, "the error value is exact");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=server_error", Reject::BadError, "no other error value");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=", Reject::BadError, "an empty error");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denied2", Reject::BadError,
               "access_denied plus a digit");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access_denie", Reject::BadError,
               "access_denied less its last letter");
}

// The page sends no character that needs an escape, so the parser has no decoder
// and refuses a '%' wherever it stands: a sign that a link is not the page's, whatever
// the escape would have decoded to.
void TestAPercentSignIsRefusedEverywhere() {
  // a bare '%', and the malformed escapes a decoder would have to decide about
  for (const char* tail : {"%", "%4", "%zz", "%-1", "%%", "%4G", "%G4", "%00", "%0a", "%20", "%7f", "%e9", "%41"}) {
    ExpectReject(kAuth + "?state=" + kState + "&error=access_denied" + tail, Reject::UnexpectedEscape,
                 std::string("the answer's last value ending in ") + tail);
    ExpectReject(kAuth + "?state=" + kState + tail + "&error=access_denied", Reject::UnexpectedEscape,
                 std::string("the state ending in ") + tail);
    ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + tail + "&n=" + kNonce + "&d=" + kData,
                 Reject::UnexpectedEscape, std::string("the epk ending in ") + tail);
  }
  // an escape that decodes to the character the page would have sent
  ExpectReject(kAuth + "?state=%53" + kState.substr(1) + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData,
               Reject::UnexpectedEscape, "an escaped character in the state of a sealed link");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=%44" + kData.substr(1),
               Reject::UnexpectedEscape, "an escaped character in the sealed data");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=access%5Fdenied", Reject::UnexpectedEscape,
               "an escaped spelling of the legal error value");
  ExpectReject("urnetwork://auth?state=%53" + kState.substr(1) + "&error=access_denied", Reject::UnexpectedEscape,
               "an escaped character in the state of a denial");
  // in a key, where a decoder might make an alias of a legal one
  ExpectReject("urnetwork://auth?%73tate=" + kState + "&error=access_denied", Reject::UnexpectedEscape,
               "a percent-encoded key is not an alias of state");
  ExpectReject("urnetwork://auth?state=" + kState + "&%65rror=access_denied", Reject::UnexpectedEscape,
               "a percent-encoded error key");
  // in the delimiters and the path
  ExpectReject("urnetwork://auth?state=" + kState + "%26error=access_denied", Reject::UnexpectedEscape,
               "an escaped '&' between the pairs");
  ExpectReject("urnetwork://auth?state%3D" + kState + "&error=access_denied", Reject::UnexpectedEscape,
               "an escaped '=' inside a pair");
  ExpectReject("urnetwork://auth%3Fstate=" + kState + "&error=access_denied", Reject::UnexpectedEscape,
               "an escaped '?' after the host");
  ExpectReject("urnetwork://auth%2F?state=" + kState + "&error=access_denied", Reject::UnexpectedEscape,
               "an escaped '/' after the host");
  ExpectReject("urnetwork://auth/%2e%2e?state=" + kState + "&error=access_denied", Reject::UnexpectedEscape,
               "an escaped dot-dot path");
  // a decoder of the old plain code would have read these: the padding, a plus
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData + "%3D",
               Reject::UnexpectedEscape, "an escaped padding character");
  ExpectReject(kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=%2541", Reject::UnexpectedEscape,
               "a double escape");

  // the percent sign is refused in the host's own place as the host is, not as an escape
  ExpectReject("urnetwork://%61uth?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "an escaped letter in the host is another host");
  ExpectReject("urnetwork%3A//auth?state=" + kState + "&error=access_denied", Reject::NotAuthHost,
               "an escaped colon in the scheme is another scheme");
}

// Every one of the 256 bytes written as an escape, in upper-case and in lower-case hex, in every
// value of every shape, and in the key place too: all are refused for the one reason. A parser that
// decoded some of them (the hex digits of one case, the printable bytes) would let one through.
void TestEveryEscapeIsRefusedAsOne() {
  int refused = 0;
  int total = 0;
  for (int byte = 0; byte < 256; ++byte) {
    char upper[16];
    char lower[16];
    std::snprintf(upper, sizeof upper, "%%%02X", byte);
    std::snprintf(lower, sizeof lower, "%%%02x", byte);
    for (const char* escape : {upper, lower}) {
      const std::string e = escape;
      const std::string links[] = {
          kAuth + "?state=" + kState.substr(1) + e + "&error=access_denied",
          kAuth + "?state=" + kState + "&epk=" + kEpk.substr(1) + e + "&n=" + kNonce + "&d=" + kData,
          kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce.substr(1) + e + "&d=" + kData,
          kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + kData.substr(1) + e,
          kAuth + "?state=" + kState + "&error=access_denie" + e,
          kAuth + "?state=" + kState + "&e" + e + "ror=access_denied",
          kAuth + "?" + e + "tate=" + kState + "&error=access_denied",
      };
      for (const std::string& link : links) {
        ++total;
        const authlink::Return r = authlink::Parse(link);
        if (r.kind == Kind::Invalid && r.reject == Reject::UnexpectedEscape) {
          ++refused;
        } else if (total - refused <= 3) {
          std::cout << "  FAIL the escape " << e << " was not refused as an escape (got " << Reason(r) << ")\n";
        }
      }
    }
  }
  Check(total == 256 * 2 * 7, "escapes: every byte, in both cases of hex, in seven places, was tried");
  Check(refused == total, "escapes: every one of " + std::to_string(total) + " is refused as an unexpected escape");
}

// The links above are the ones that were thought of. This is the rest: take each
// accepted link, put one hostile character in each place in turn, and none may
// still be a link.
void TestOneHostileCharacterAnywhereMakesItNoLink() {
  // NUL, controls, every delimiter, the escape and padding characters, and bytes above 0x7E
  static const char kHostile[] = " \t\r\n\0%&=?#/@:+\\\"<>,;'\x7f\x80\xff";
  const std::string hostile(kHostile, sizeof(kHostile) - 1);
  const std::string samples[] = {Sealed(), Sealed(true), Denied(), Denied(true)};
  int tried = 0;
  int slipped = 0;
  for (const std::string& sample : samples) {
    for (std::size_t i = 0; i < sample.size(); ++i) {
      for (const char bad : hostile) {
        if (bad == sample[i]) continue;
        std::string mutated = sample;
        mutated[i] = bad;
        ++tried;
        if (authlink::Parse(mutated).kind != Kind::Invalid) {
          if (++slipped <= 3) {
            std::cout << "  FAIL a link with character " << i << " replaced by 0x" << std::hex
                      << static_cast<int>(static_cast<unsigned char>(bad)) << std::dec << " was accepted (shape "
                      << sample.substr(0, 40) << "...)\n";
          }
        }
      }
    }
  }
  Check(tried > 40000, "hostile characters: the sweep covered every place for every character");
  Check(slipped == 0, "hostile characters: none of " + std::to_string(tried) + " altered links was accepted");
}

std::string SealedWith(const std::string& state, const std::string& epk, const std::string& nonce,
                       const std::string& data, bool slash = false) {
  return kAuth + (slash ? "/" : "") + "?state=" + state + "&epk=" + epk + "&n=" + nonce + "&d=" + data;
}

std::string DeniedFor(const std::string& state, bool slash = false) {
  return kAuth + (slash ? "/" : "") + "?state=" + state + "&error=access_denied";
}

// A sweep tries a great many links, so it counts the ones the parser got wrong, prints the
// first few and leaves one check for the lot, as the sweep of hostile characters does.
struct Sweep {
  int tried = 0;
  int wrong = 0;

  void Expect(bool right, const std::string& what) {
    ++tried;
    if (!right && ++wrong <= 3) std::cout << "  FAIL " << what << std::endl;  // flushed: a spec that dies a moment later still says what failed
  }

  void Conclude(const std::string& what) const {
    Check(wrong == 0, what + " (" + std::to_string(wrong) + " of " + std::to_string(tried) + " links wrong)");
  }
};

// The page's conformance vector, as far as the parser reads it (contract 1; approval-vector.h has the
// three values and says how the page made them, the manager's tests/fixtures/app-auth-vector.json
// the whole address). The page sealed a made-up code with a one-time key and a nonce that are fixed
// and not secret, so these are real base58 of the real lengths with every kind of character in
// them. Opening the box takes the sdk's crypto and is not done here (tools/approval-interop-tests.cpp
// does it).
const std::string kPageEpk(approval_vector::kEpk);
const std::string kPageNonce(approval_vector::kNonce);
const std::string kPageData(approval_vector::kData);

void TestThePagesOwnAnswersAreRead() {
  Check(kPageEpk.size() == kEpk.size() && kPageNonce.size() == kNonce.size() && kPageData.size() == kData.size(),
        "page vector: the key, the nonce and the box are 44, 33 and 1,036 characters");
  // state, epk, n, d in that order, as the page builds it; its own test pins the length of the address
  const std::string approve = SealedWith(kState, kPageEpk, kPageNonce, kPageData);
  Check(approve.size() == 1190, "page vector: the approve address is the 1,190 characters the page pins");
  authlink::Return r = authlink::Parse(approve);
  Check(r.kind == Kind::Sealed && r.reject == Reject::None && r.state == kState && r.epk == kPageEpk &&
            r.nonce == kPageNonce && r.data == kPageData,
        "page vector: the approve address is read, value for value (got " + Reason(r) + ")");
  r = authlink::Parse(SealedWith(kState, kPageEpk, kPageNonce, kPageData, /*slash=*/true));
  Check(r.kind == Kind::Sealed && r.epk == kPageEpk && r.nonce == kPageNonce && r.data == kPageData,
        "page vector: the approve address with the browser's '/' is read (got " + Reason(r) + ")");
  const std::string deny = "urnetwork://auth?state=" + std::string(approval_vector::kState) + "&error=access_denied";
  Check(deny == Denied(), "page vector: the deny address is the one the fixtures above build");
  r = authlink::Parse(deny);
  Check(r.kind == Kind::Denied && r.state == kState, "page vector: the deny address is read (got " + Reason(r) + ")");
  r = authlink::Parse(DeniedFor(kState, /*slash=*/true));
  Check(r.kind == Kind::Denied, "page vector: the deny address with the browser's '/' is read (got " + Reason(r) + ")");
}

// The links above are made of one repeated capital letter, which no real value is: a state is
// 32 random bytes in base64url, so digits, lower case, '-' and '_' are among its characters,
// and the sealed values are base58, which has every digit and letter but 0, O, I and l. A
// parser that took capitals only would pass every check above and then refuse every link the
// page sends. So the two alphabets are spelled out here, not read off the header, each of their
// characters is put in each place of a value in turn, and every other printable character is
// refused where it stands.
const std::string kBase64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
// Bitcoin's alphabet, which the sdk's encodeBase58 and the page's bs58 both use.
const std::string kBase58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

// `length` characters of `alphabet` from place `offset` on, wrapping round. Over all the offsets
// each place of a value holds each character of the alphabet, even in a value shorter than the
// alphabet: a 43-character state cannot hold all 64 base64url characters at once.
std::string Rotated(const std::string& alphabet, std::size_t offset, std::size_t length) {
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) out.push_back(alphabet[(offset + i) % alphabet.size()]);
  return out;
}

std::string With(std::string value, std::size_t at, char c) {
  value[at] = c;
  return value;
}

// The first, a middle and the last place of a value of `size` characters.
std::vector<std::size_t> Places(std::size_t size) { return {0, size / 2, size - 1}; }

// The printable characters outside `alphabet`, less the four that shape a link and fail for
// their own reasons (the sweep of hostile characters above puts those everywhere): '&' splits
// a pair, '?' and '#' are delimiters, '%' is refused as an escape wherever it stands.
std::string Outsiders(const std::string& alphabet) {
  std::string out;
  for (int c = 0x21; c < 0x7F; ++c) {
    const char ch = static_cast<char>(c);
    if (alphabet.find(ch) == std::string::npos && std::string_view("&?#%").find(ch) == std::string_view::npos) {
      out.push_back(ch);
    }
  }
  return out;
}

void TestTheSpecsAlphabetsAreTheStandardOnes() {
  std::string base64url;
  for (char c = 'A'; c <= 'Z'; ++c) base64url.push_back(c);
  for (char c = 'a'; c <= 'z'; ++c) base64url.push_back(c);
  for (char c = '0'; c <= '9'; ++c) base64url.push_back(c);
  base64url += "-_";
  Check(kBase64Url == base64url, "alphabets: base64url is A-Z, a-z, 0-9, '-' and '_' (RFC 4648, section 5)");
  std::string base58;
  for (char c = '1'; c <= '9'; ++c) base58.push_back(c);
  for (char c = 'A'; c <= 'Z'; ++c) {
    if (c != 'I' && c != 'O') base58.push_back(c);
  }
  for (char c = 'a'; c <= 'z'; ++c) {
    if (c != 'l') base58.push_back(c);
  }
  Check(kBase58 == base58 && kBase58.size() == 58, "alphabets: base58 is Bitcoin's 58 characters, without 0, O, I and l");
}

void TestEveryCharacterOfEachAlphabetIsRead() {
  // a state of every base64url character in every place, sealed and denied, with and without the '/'
  Sweep base64url;
  for (std::size_t offset = 0; offset < kBase64Url.size(); ++offset) {
    const std::string state = Rotated(kBase64Url, offset, authlink::kStateChars);
    for (const bool slash : {false, true}) {
      const std::string where = "at rotation " + std::to_string(offset) + (slash ? " with the '/'" : "");
      authlink::Return r = authlink::Parse(DeniedFor(state, slash));
      base64url.Expect(r.kind == Kind::Denied && r.state == state,
                       "a denial " + where + " was not read (got " + Reason(r) + ")");
      r = authlink::Parse(SealedWith(state, kEpk, kNonce, kData, slash));
      base64url.Expect(r.kind == Kind::Sealed && r.state == state,
                       "a sealed answer " + where + " was not read (got " + Reason(r) + ")");
    }
  }
  base64url.Conclude("alphabets: every base64url character is read in every place of a state");

  // a key, a nonce and a box of every base58 character in every place
  Sweep base58;
  for (std::size_t offset = 0; offset < kBase58.size(); ++offset) {
    const std::string state = Rotated(kBase64Url, offset, authlink::kStateChars);
    const std::string epk = Rotated(kBase58, offset, kEpk.size());
    const std::string nonce = Rotated(kBase58, offset, kNonce.size());
    const std::string data = Rotated(kBase58, offset, kData.size());
    for (const bool slash : {false, true}) {
      const authlink::Return r = authlink::Parse(SealedWith(state, epk, nonce, data, slash));
      base58.Expect(r.kind == Kind::Sealed && r.state == state && r.epk == epk && r.nonce == nonce && r.data == data,
                    "a sealed link at rotation " + std::to_string(offset) + (slash ? " with the '/'" : "") +
                        " was not read (got " + Reason(r) + ")");
    }
  }
  base58.Conclude("alphabets: every base58 character is read in every place of a key, a nonce and a box");
}

void TestNoCharacterOutsideAnAlphabetIsRead() {
  // one value of each kind, made of its alphabet and so a link as it stands; each check puts one outsider in it
  const std::string state = Rotated(kBase64Url, 0, authlink::kStateChars);
  const std::string epk = Rotated(kBase58, 3, kEpk.size());
  const std::string nonce = Rotated(kBase58, 5, kNonce.size());
  const std::string data = Rotated(kBase58, 11, kData.size());

  Sweep outsiders;
  const auto expect = [&outsiders](const std::string& url, Reject why, const std::string& what) {
    const authlink::Return r = authlink::Parse(url);
    outsiders.Expect(r.kind == Kind::Invalid && r.reject == why, what + " (got " + Reason(r) + ")");
  };
  for (const char c : Outsiders(kBase64Url)) {
    const std::string name = std::string("'") + c + "'";
    for (const std::size_t at : Places(state.size())) {
      const std::string bad = With(state, at, c);
      const std::string what = "a state with " + name + " at place " + std::to_string(at);
      expect(DeniedFor(bad), Reject::BadState, what + ", denied");
      expect(SealedWith(bad, epk, nonce, data), Reject::BadState, what + ", sealed");
    }
  }
  for (const char c : Outsiders(kBase58)) {
    const std::string name = std::string("'") + c + "'";
    for (const std::size_t at : Places(epk.size())) {
      expect(SealedWith(state, With(epk, at, c), nonce, data), Reject::BadValue,
             "an epk with " + name + " at place " + std::to_string(at));
    }
    for (const std::size_t at : Places(nonce.size())) {
      expect(SealedWith(state, epk, With(nonce, at, c), data), Reject::BadValue,
             "a nonce with " + name + " at place " + std::to_string(at));
    }
    for (const std::size_t at : Places(data.size())) {
      expect(SealedWith(state, epk, nonce, With(data, at, c)), Reject::BadValue,
             "a box with " + name + " at place " + std::to_string(at));
    }
  }
  // and the percent sign, in the same places, is an escape and nothing else
  for (const std::size_t at : Places(state.size())) {
    expect(DeniedFor(With(state, at, '%')), Reject::UnexpectedEscape, "a state with '%' at place " + std::to_string(at));
  }
  for (const std::size_t at : Places(data.size())) {
    expect(SealedWith(state, epk, nonce, With(data, at, '%')), Reject::UnexpectedEscape,
           "a box with '%' at place " + std::to_string(at));
  }
  outsiders.Conclude("alphabets: every printable character outside an alphabet is refused where it stands");
}

void TestBase58LeavesOutTheLookalikes() {
  // 0, O, I and l look like base58 characters and are none: refuse each on its own, in every sealed value
  const std::string state = Rotated(kBase64Url, 0, authlink::kStateChars);
  const std::string epk = Rotated(kBase58, 3, kEpk.size());
  const std::string nonce = Rotated(kBase58, 5, kNonce.size());
  const std::string data = Rotated(kBase58, 11, kData.size());
  for (const char left : std::string("0OIl")) {
    const std::string bad(1, left);
    ExpectReject(SealedWith(state, epk.substr(1) + bad, nonce, data), Reject::BadValue,
                 std::string("base58 leaves out ") + left + " (epk)");
    ExpectReject(SealedWith(state, epk, nonce.substr(1) + bad, data), Reject::BadValue,
                 std::string("base58 leaves out ") + left + " (n)");
    ExpectReject(SealedWith(state, epk, nonce, data.substr(1) + bad), Reject::BadValue,
                 std::string("base58 leaves out ") + left + " (d)");
  }
}

// The parser's caps, at their edges: the url as a whole, the number of pairs, and a value. Each is the only thing
// wrong with the link in the cases below, or else the reason given for refusing it would not be this one.
void TestTheCapsAreTheContractsOwnToTheByte() {
  // the whole address: 4096 bytes are read (and found to be no answer for another reason), 4097 are too long
  const std::string head = kAuth + "?state=" + kState + "&epk=" + kEpk + "&n=" + kNonce + "&d=" + std::string(2048, 'D') + "&error=";
  Check(head.size() < 4096 - 10, "caps: the control link has room for a value of its own");
  const std::string atTheCap = head + std::string(4096 - head.size(), 'a');
  const std::string pastTheCap = head + std::string(4097 - head.size(), 'a');
  Check(atTheCap.size() == 4096 && pastTheCap.size() == 4097, "caps: the two links are 4096 and 4097 bytes");
  ExpectReject(atTheCap, Reject::MixedShapes, "an address of exactly the cap is read, and is no answer (an error beside a sealed one)");
  ExpectReject(pastTheCap, Reject::TooLong, "an address one byte past the cap is too long, before anything else is looked at");
  // the pairs: six are read (the unknown one is what is wrong), seven are too many before any key is
  ExpectReject(Sealed() + "&a=1&b=2", Reject::UnknownKey, "six pairs are not over the cap");
  ExpectReject(Sealed() + "&a=1&b=2&c=3", Reject::TooLong, "seven pairs are");
  ExpectReject("urnetwork://auth?a=1&b=2&c=3&d=4&e=5&f=6&g=7", Reject::TooLong, "seven pairs are, whatever the keys");
  ExpectReject("urnetwork://auth?a=1&b=2&c=3&d=4&e=5&f=6", Reject::UnknownKey, "and six unknown ones are unknown, not too many");
  // a value: 2048 bytes are read, 2049 are too long
  ExpectReject(kAuth + "?state=" + kState + "&error=" + std::string(2048, 'a'), Reject::BadError, "a value of exactly the cap is read");
  ExpectReject(kAuth + "?state=" + kState + "&error=" + std::string(2049, 'a'), Reject::TooLong, "a value one byte past the cap is too long");
}

void TestTheOtherContracts() {
  const authlink::Contract strictPath{authlink::kAuthPrefix, false, 4096, 2048, 6};
  ExpectReject(Denied(/*slash=*/true), Reject::Path, "no tolerated '/': urnetwork://auth/? is a path", strictPath);
  authlink::Return r = authlink::Parse(Denied(), strictPath);
  Check(r.kind == Kind::Denied, "no tolerated '/': the plain form is still read");
  r = authlink::Parse(Sealed(), strictPath);
  Check(r.kind == Kind::Sealed, "no tolerated '/': a sealed answer is still read");

  const authlink::Contract small{authlink::kAuthPrefix, true, 200, 50, 3};
  ExpectReject(Sealed(), Reject::TooLong, "a contract's own url cap", small);
  r = authlink::Parse("urnetwork://auth?state=" + kState + "&error=access_denied", small);
  Check(r.kind == Kind::Denied, "a contract's smaller caps still read a short link");
  ExpectReject("urnetwork://auth?state=" + kState + "&error=" + std::string(60, 'a'), Reject::TooLong,
               "a contract's own value cap", small);
  ExpectReject("urnetwork://auth?state=" + kState + "&epk=a&n=b&d=c", Reject::TooLong,
               "a contract's own cap on the pairs", small);

  const authlink::Contract other{"urnetwork://other", true, 4096, 2048, 6};
  ExpectReject(Denied(), Reject::NotAuthHost, "a contract's own prefix", other);
  r = authlink::Parse("urnetwork://other?state=" + kState + "&error=access_denied", other);
  Check(r.kind == Kind::Denied, "a contract's own prefix is read");
}

void TestRoutingSendsOnlyTheAuthHostToTheGate() {
  Check(authlink::IsAuthHost("urnetwork://auth"), "route: the bare host");
  Check(authlink::IsAuthHost("urnetwork://auth?x=1"), "route: '?' after the host");
  Check(authlink::IsAuthHost("urnetwork://auth/?x=1"), "route: '/' after the host");
  Check(authlink::IsAuthHost("urnetwork://auth#x"), "route: '#' after the host");
  Check(authlink::IsAuthHost("urnetwork://auth/x/y"), "route: a path under the host is still the auth host's (and a stray)");
  Check(authlink::IsAuthHost(Sealed()) && authlink::IsAuthHost(Sealed(true)) && authlink::IsAuthHost(Denied()),
        "route: both shapes");
  // the case of the scheme and the host does not decide whether a link reaches the gate; Parse then
  // refuses it, so a variant spelling is a stray and not a link that raises the window
  Check(authlink::IsAuthHost("URNETWORK://auth?x=1"), "route: an upper-case scheme");
  Check(authlink::IsAuthHost("urnetwork://AUTH?x=1"), "route: an upper-case host");
  Check(authlink::IsAuthHost("UrNetwork://aUtH/?x=1"), "route: a mixed-case scheme and host");
  Check(authlink::IsAuthHost("URNETWORK://AUTH"), "route: the bare host in capitals");
  Check(authlink::IsAuthHost("URNETWORK://AUTH#x"), "route: '#' after a host in capitals");
  Check(authlink::Parse("URNETWORK://AUTH?state=" + kState + "&error=access_denied").reject == Reject::NotAuthHost,
        "route: and the gate's parser calls such a link not-auth-host");
  Check(!authlink::IsAuthHost("urnetwork://authenticator/x"), "route: authenticator is another host");
  Check(!authlink::IsAuthHost("URNETWORK://AUTHENTICATOR/x"), "route: AUTHENTICATOR is another host");
  Check(!authlink::IsAuthHost("urnetwork://auth.evil"), "route: auth.evil is another host");
  Check(!authlink::IsAuthHost("URNETWORK://AUTH.EVIL"), "route: AUTH.EVIL is another host");
  Check(!authlink::IsAuthHost("urnetwork://auth@evil"), "route: userinfo after the host is another host");
  Check(!authlink::IsAuthHost("urnetwork://auth:80/"), "route: a port after the host is another host");
  Check(!authlink::IsAuthHost("urnetwork://oauth/google"), "route: oauth is another host");
  Check(!authlink::IsAuthHost("urnetwork://onboarding/feedback"), "route: the campaign links are not auth");
  Check(!authlink::IsAuthHost("urnetwork://phantom-connect?x=1"), "route: the wallet returns are not auth");
  Check(!authlink::IsAuthHost("urnetwork://%61uth?x=1"), "route: an escaped letter is another host");
  Check(!authlink::IsAuthHost("urnetwork:auth?x=1") && !authlink::IsAuthHost("urnetwork:/auth?x=1"),
        "route: the forms without the slashes are no link of this scheme");
  Check(!authlink::IsAuthHost("urnetwork://au"), "route: a prefix of the host");
  // the prefix is compared in full: a host as long as "auth" with the right character after the sixteenth
  // is another host, whatever follows it (a router that read only that character would send it to the gate)
  Check(!authlink::IsAuthHost("urnetwork://link") && !authlink::IsAuthHost("urnetwork://link?x=1") &&
            !authlink::IsAuthHost("urnetwork://link/x") && !authlink::IsAuthHost("urnetwork://link#x"),
        "route: a four-letter host that is not auth is not the auth host, bare or followed by '?', '/' or '#'");
  Check(!authlink::IsAuthHost("xxxxxxxxxxxxxxxx") && !authlink::IsAuthHost("xxxxxxxxxxxxxxxx?x=1") &&
            !authlink::IsAuthHost("https://ur.io/auth?") && !authlink::IsAuthHost("urnetwork:///auth?x=1"),
        "route: sixteen characters of anything else, then '?', is not the auth host either");
  Check(authlink::IsAuthHost("urnetwork://auth") && !authlink::IsAuthHost("urnetwork://aut"),
        "route: the bare host is exactly sixteen characters, and fifteen are not enough");
  Check(!authlink::IsAuthHost("https://ur.io/auth") && !authlink::IsAuthHost(""), "route: another scheme, and nothing");
}

void TestTheTokensAreClosed() {
  const Reject all[] = {
      Reject::None,        Reject::NotAuthHost, Reject::Path,        Reject::Authority,   Reject::Delimiter,
      Reject::TooLong,     Reject::Control,     Reject::UnexpectedEscape, Reject::EmptyPair, Reject::Duplicate,
      Reject::UnknownKey,  Reject::MixedShapes, Reject::BadState,    Reject::BadValue,    Reject::BadError,
  };
  std::set<std::string> seen;
  bool hyphenated = true;
  bool lowercase = true;
  for (const Reject reject : all) {
    const std::string token(authlink::ToString(reject));
    seen.insert(token);
    hyphenated = hyphenated && token.find('-') != std::string::npos;
    for (const char c : token) lowercase = lowercase && ((c >= 'a' && c <= 'z') || c == '-');
  }
  Check(seen.size() == std::size(all), "tokens: one per reason, none repeated");
  Check(hyphenated, "tokens: every one is hyphenated, so none can be the id of a string resource");
  Check(lowercase, "tokens: lowercase letters and hyphens only");
  Check(authlink::ToString(Reject::UnexpectedEscape) == "unexpected-escape", "tokens: unexpected-escape");
  Check(authlink::kStateChars == 43, "state: 43 characters, the base64url of 32 bytes");
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void TestALinkInACommandLineIsFoundWhateverItsCase() {
  const std::wstring exe = LR"("C:\Program Files\URnetwork\URnetwork.exe")";
  const std::wstring link = L"urnetwork://auth/?state=SECRETSTATE&error=access_denied";
  // quoted, as the shell passes %1, and bare, at the end of the line
  Check(authlink::LinkInCommandLine(exe + L" \"" + link + L"\"") == link, "command line: a quoted link, without its quotes");
  Check(authlink::LinkInCommandLine(exe + L" " + link) == link, "command line: a link at the end of the line");
  Check(authlink::LinkInCommandLine(exe + L" " + link + L" --autostart") == link,
        "command line: a link ends at the space before the next argument");
  Check(authlink::LinkInCommandLine(exe + L" " + link + L"\t--autostart") == link,
        "command line: a link ends at a tab");
  Check(authlink::LinkInCommandLine(exe + L" --uri=" + link) == link, "command line: a link glued to a flag");
  Check(authlink::LinkInCommandLine(link + L" " + L"urnetwork://oauth/x") == link, "command line: the first of two links");
  // the case of the scheme does not hide a link
  Check(authlink::LinkInCommandLine(L"x URNETWORK://auth?a=b") == L"URNETWORK://auth?a=b",
        "command line: an upper-case scheme is a link");
  Check(authlink::LinkInCommandLine(L"x UrNetwork://AUTH?a=b y") == L"UrNetwork://AUTH?a=b",
        "command line: a mixed-case scheme and host are a link");
  // and what is no link of this scheme is none
  Check(authlink::LinkInCommandLine(exe + L" --autostart").empty(), "command line: no link");
  Check(authlink::LinkInCommandLine(L"").empty(), "command line: an empty line");
  Check(authlink::LinkInCommandLine(L"urnetwork:auth?a=b").empty() && authlink::LinkInCommandLine(L"urnetwork:/auth?a=b").empty(),
        "command line: the forms without the slashes are not what the shell passes");
  Check(authlink::LinkInCommandLine(L"urnetwork//auth").empty() && authlink::LinkInCommandLine(L"urnetwork:/").empty(),
        "command line: a scheme that stops short is no link");
  Check(authlink::LinkInCommandLine(L"urnetwork://") == L"urnetwork://", "command line: a scheme alone is a link to nothing");
  // a link the router finds is one the redaction removes whole: never a byte of it is left
  for (const std::wstring& line :
       std::vector<std::wstring>{exe + L" \"" + link + L"\"", exe + L" " + link, exe + L" --uri=" + link,
                                 L"x URNETWORK://auth?a=SECRETB y", L"UrNetwork://AUTH?a=SECRETB"}) {
    const std::wstring_view found = authlink::LinkInCommandLine(line);
    const std::wstring redacted = authlink::RedactCommandLine(line);
    Check(!found.empty() && redacted.find(L"SECRET") == std::wstring::npos &&
              redacted.find(std::wstring(found)) == std::wstring::npos,
          "command line: the link the router finds is not in the redacted line");
  }
}

void TestRedaction() {
  // a url
  Check(authlink::RedactUrl("urnetwork://auth?a=b") == "urnetwork://auth [redacted 20 chars]",
        "redact url: the scheme, the host and a length");
  const std::string redactedSealed = authlink::RedactUrl(Sealed(true));
  Check(redactedSealed == "urnetwork://auth [redacted " + std::to_string(Sealed(true).size()) + " chars]",
        "redact url: a sealed return is reduced to the same shape");
  for (const std::string& value : {kState, kEpk, kNonce, kData}) {
    Check(redactedSealed.find(value) == std::string::npos, "redact url: no value survives");
  }
  Check(authlink::RedactUrl("urnetwork://user:SECRETPW@host/?x=1").find("SECRETPW") == std::string::npos,
        "redact url: a userinfo password never survives");
  Check(authlink::RedactUrl("urnetwork://SECRETTOKEN@host/?x=1").find("SECRETTOKEN") == std::string::npos,
        "redact url: a userinfo token never survives either");
  Check(authlink::RedactUrl("urnetwork://auth:SECRETPORT/?x=1").find("SECRETPORT") == std::string::npos,
        "redact url: nor does what follows a colon");
  Check(authlink::RedactUrl("urnetwork://SECRETHOST.example.com.SECRETHOST.example.com.x?x=1").find("SECRET") ==
            std::string::npos,
        "redact url: a long host is not echoed");
  Check(authlink::RedactUrl("urnetwork://au\nth?x=1").find('\n') == std::string::npos,
        "redact url: a host with a line break is not echoed");
  Check(authlink::RedactUrl("urnetwork://phantom-connect?x=1").rfind("urnetwork://phantom-connect ", 0) == 0,
        "redact url: the wallet bridges' hosts are kept");
  Check(authlink::RedactUrl("urnetwork://solflare-sign-message?x=1").rfind("urnetwork://solflare-sign-message ", 0) == 0,
        "redact url: the longest host the app routes is kept");
  Check(authlink::RedactUrl("urnetwork://") == "urnetwork:// [redacted 12 chars]", "redact url: a scheme alone");
  Check(authlink::RedactUrl("") == "[redacted 0 chars]", "redact url: nothing");
  Check(authlink::RedactUrl("SECRET text with spaces") == "[redacted 23 chars]", "redact url: not a url, not echoed");
  Check(authlink::RedactUrl("sec ret://x/?y=1").find("sec") == std::string::npos,
        "redact url: a scheme that is not a plain token is not echoed");
  Check(authlink::RedactUrl("urnetwork:auth?state=SECRETSTATE") == "[redacted 32 chars]",
        "redact url: a link without its slashes is a length and nothing else");

  // a command line
  const std::wstring exe = LR"("C:\Program Files\URnetwork\URnetwork.exe")";
  const std::wstring link = L"urnetwork://auth/?state=SECRETSTATE&error=access_denied";
  std::wstring redacted = authlink::RedactCommandLine(exe + L" \"" + link + L"\"");
  Check(redacted.find(L"SECRET") == std::wstring::npos, "redact line: no value survives");
  Check(redacted == exe + L" \"urnetwork://auth [redacted " + std::to_wstring(link.size()) + L" chars]\"",
        "redact line: the exe path and the quotes are kept, the link is the host and a length");
  const std::string sealedLink = Sealed(true);
  redacted = authlink::RedactCommandLine(exe + L" " + Wide(sealedLink));
  Check(redacted.find(Wide(kState)) == std::wstring::npos && redacted.find(Wide(kData)) == std::wstring::npos,
        "redact line: a sealed return in a command line leaves nothing of itself");
  redacted = authlink::RedactCommandLine(L"URNETWORK.EXE URNETWORK://oauth/google?state=x&id_token=SECRETTOKEN");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted.find(L"URNETWORK://oauth [redacted ") != std::wstring::npos,
        "redact line: the scheme is matched whatever its case; an unquoted link runs to the end");
  Check(authlink::RedactCommandLine(L"\"C:\\x\\URnetwork.exe\" --autostart") == L"\"C:\\x\\URnetwork.exe\" --autostart",
        "redact line: a command line with no link is returned as it is");
  Check(authlink::RedactCommandLine(L"") == L"", "redact line: an empty line");
  redacted = authlink::RedactCommandLine(
      L"a urnetwork://onboarding/feedback?token=SECRETONE b \"urnetwork://auth?code=SECRETTWO\" --autostart");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted.find(L" b ") != std::wstring::npos &&
            redacted.find(L" --autostart") != std::wstring::npos,
        "redact line: two links, the text between them and after them kept");
  redacted = authlink::RedactCommandLine(L"x urnetwork://auth?code=\u00e9SECRET");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted.find(L'\u00e9') == std::wstring::npos,
        "redact line: a non-ASCII character in a link does not break the redaction");
  Check(authlink::RedactCommandLine(L"urnetwork://") == L"urnetwork:// [redacted 12 chars]",
        "redact line: a scheme alone at the end");

  // the forms without the slashes: the app does not act on them, and a log must not carry them
  redacted = authlink::RedactCommandLine(exe + L" urnetwork:auth?state=SECRETSTATE&code=SECRETCODE");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted.find(exe) == 0,
        "redact line: urnetwork:auth?... leaves no value");
  redacted = authlink::RedactCommandLine(exe + L" urnetwork:/auth?state=SECRETSTATE&code=SECRETCODE");
  Check(redacted.find(L"SECRET") == std::wstring::npos, "redact line: urnetwork:/auth?... leaves no value");
  redacted = authlink::RedactCommandLine(L"URNETWORK:AUTH?state=SECRETSTATE y");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted == L"[redacted 32 chars] y",
        "redact line: URNETWORK:AUTH?... in capitals leaves no value, and the text after it stays");
  redacted = authlink::RedactCommandLine(L"--uri=urnetwork:auth?state=SECRETSTATE");
  Check(redacted.find(L"SECRET") == std::wstring::npos && redacted.rfind(L"--uri=", 0) == 0,
        "redact line: a link glued to a flag, without its slashes");
  redacted = authlink::RedactCommandLine(L"\"urnetwork:/auth?state=SECRETSTATE\"");
  Check(redacted == L"\"[redacted 33 chars]\"", "redact line: a quoted link without its slashes");
}

// The log of a build before the redaction holds the whole command line of every protocol launch;
// exporting it copies that, so the export reduces each link in it the same way (RedactLogText).
void TestLoggedTextIsRedactedLineByLine() {
  const std::string exe = R"("C:\Program Files\URnetwork\URnetwork.exe")";
  const std::string link = "urnetwork://auth/?state=SECRETSTATE&epk=SECRETKEY&n=SECRETNONCE&d=SECRETBOX";
  const std::string tag = "urnetwork://auth [redacted " + std::to_string(link.size()) + " chars]";
  const std::string stamp = "2026-10-07 23:36:01 INFO  startup:   command line     : ";
  // a quoted link, with the CRLF a log line ends in
  std::string redacted = authlink::RedactLogText(stamp + exe + " \"" + link + "\"\r\n");
  Check(redacted == stamp + exe + " \"" + tag + "\"\r\n", "log text: a quoted link is its host and a length, the line break kept");
  // an unquoted link at the end of its line: the line break is not part of it
  redacted = authlink::RedactLogText(stamp + exe + " " + link + "\r\nnext line\r\n");
  Check(redacted == stamp + exe + " " + tag + "\r\nnext line\r\n", "log text: a link ends at the end of its line (CRLF)");
  redacted = authlink::RedactLogText("a " + link + "\nb " + link + "\n");
  Check(redacted == "a " + tag + "\nb " + tag + "\n", "log text: and at a bare line feed, on every line");
  redacted = authlink::RedactLogText("end " + link);
  Check(redacted == "end " + tag, "log text: a link at the very end of the text");
  Check(redacted.find("SECRET") == std::string::npos, "log text: no value survives");

  // the forms the app does not act on, and any case
  redacted = authlink::RedactLogText("x urnetwork:auth?state=SECRETSTATE\r\ny URNETWORK:/AUTH?code=SECRETCODE\r\n");
  Check(redacted.find("SECRET") == std::string::npos && redacted.find("\r\n") != std::string::npos &&
            redacted.rfind("y ", redacted.find("\r\n") + 2) != std::string::npos,
        "log text: the forms without the slashes, in any case, leave no value and keep the lines");
  const std::string mixed = "UrNetwork://Auth?state=SECRETSTATE";
  redacted = authlink::RedactLogText("x " + mixed + " y");
  Check(redacted == "x UrNetwork://Auth [redacted " + std::to_string(mixed.size()) + " chars] y",
        "log text: a mixed-case link, and what follows it");

  // several links on a line, text between them kept
  redacted = authlink::RedactLogText("a urnetwork://oauth/google?id_token=SECRETONE b \"urnetwork://auth?state=SECRETTWO\" c\r\n");
  Check(redacted.find("SECRET") == std::string::npos && redacted.find(" b ") != std::string::npos &&
            redacted.find(" c\r\n") != std::string::npos,
        "log text: two links on a line, the text between and after them kept");

  // bytes above 0x7F (UTF-8) inside a link are counted and not echoed
  redacted = authlink::RedactLogText("x urnetwork://auth?code=\xC3\xA9SECRET\xE2\x82\xAC y");
  Check(redacted.find("SECRET") == std::string::npos && redacted.find('\xC3') == std::string::npos &&
            redacted.find(" y") != std::string::npos,
        "log text: UTF-8 inside a link does not break the redaction");
  // and outside one they are kept as they are
  Check(authlink::RedactLogText("caf\xC3\xA9 \xE2\x82\xAC\r\n") == "caf\xC3\xA9 \xE2\x82\xAC\r\n", "log text: UTF-8 outside a link is kept");

  // a text with no link is returned as it is
  Check(authlink::RedactLogText("") == "", "log text: nothing");
  Check(authlink::RedactLogText("a log line with no link in it\r\n") == "a log line with no link in it\r\n",
        "log text: no link, no change");
  Check(authlink::RedactLogText("urnetwork is the name, urnetwork. is not a scheme") == "urnetwork is the name, urnetwork. is not a scheme",
        "log text: the name alone is not a link");
  Check(authlink::RedactLogText("urnetwork:") == "urnetwork: [redacted 10 chars]" || authlink::RedactLogText("urnetwork:") == "[redacted 10 chars]",
        "log text: a scheme alone is redacted whole");

  // the two spellings of the redaction agree on a line with no break in it
  for (const std::string& text :
       {std::string("x ") + link + " y", std::string("\"") + link + "\"", std::string("urnetwork:auth?z=SECRETZ"),
        std::string("--uri=URNETWORK://AUTH?q=1 --autostart"), std::string("no link at all")}) {
    const std::wstring wide(text.begin(), text.end());
    const std::wstring viaLine = authlink::RedactCommandLine(wide);
    const std::string viaLog = authlink::RedactLogText(text);
    Check(viaLine == std::wstring(viaLog.begin(), viaLog.end()),
          "log text: the redaction of a log and of a command line are the same for the same text");
  }

  // a long log: many lines, a link on some of them, and no secret anywhere in what comes out
  std::string log;
  std::string expected;
  for (int i = 0; i < 2000; ++i) {
    const std::string line = "line " + std::to_string(i) + (i % 7 == 0 ? " " + link : std::string(" nothing here")) + "\r\n";
    log += line;
    expected += "line " + std::to_string(i) + (i % 7 == 0 ? " " + tag : std::string(" nothing here")) + "\r\n";
  }
  Check(authlink::RedactLogText(log) == expected, "log text: a long log, every link on every line reduced and every other byte kept");
}

// A log line the app redacted at the source (Startup.cpp) reads "urnetwork://auth [redacted 412 chars]", and its
// first half looks like a link: exported, it must not gain a second, false length in front of the real one. Only
// exactly what RedactUrl writes is taken for its own work, so no link is let through by spelling like one.
void TestARedactionIsNotRedactedAgain() {
  const std::string already = "urnetwork://auth [redacted 412 chars]";
  const std::string link = "urnetwork://auth/?state=SECRETSTATE&epk=SECRETKEY&n=SECRETNONCE&d=SECRETBOX";
  const std::string tag = "urnetwork://auth [redacted " + std::to_string(link.size()) + " chars]";
  const std::string exe = R"("C:\Program Files\URnetwork\URnetwork.exe")";
  const std::string line = "2026-10-09 12:00:00 INFO  startup:   command line     : " + exe + " " + already + "\r\n";
  Check(authlink::RedactLogText(line) == line, "again: a line redacted at the source is exported as it is");
  Check(authlink::RedactLogText("a " + already + " b " + link + " c") == "a " + already + " b " + tag + " c",
        "again: the redacted one is kept and a real link beside it is still reduced");
  Check(authlink::RedactLogText("a " + link + " b " + already) == "a " + tag + " b " + already, "again: in either order");
  Check(authlink::RedactLogText("urnetwork://* [redacted 30 chars]") == "urnetwork://* [redacted 30 chars]",
        "again: the '*' RedactUrl writes for a host it will not echo is its own too");
  Check(authlink::RedactLogText("urnetwork:// [redacted 12 chars]") == "urnetwork:// [redacted 12 chars]",
        "again: and so is a link with no host at all");
  Check(authlink::RedactLogText("URNETWORK://AUTH [redacted 5 chars]") == "URNETWORK://AUTH [redacted 5 chars]",
        "again: whatever the case of the scheme it echoed");
  Check(authlink::RedactCommandLine(L"a urnetwork://auth [redacted 412 chars] b") == L"a urnetwork://auth [redacted 412 chars] b",
        "again: a command line is the same");
  // words that only look like it are no redaction
  const auto reduced = [](const std::string& text) { return authlink::RedactLogText(text); };
  Check(reduced("urnetwork://auth?code=SECRETCODE [redacted 5 chars]").find("SECRET") == std::string::npos,
        "again: a link with its query, followed by such words, is still reduced");
  Check(reduced("urnetwork://auth/x [redacted 9 chars]") == "urnetwork://auth [redacted 18 chars] [redacted 9 chars]",
        "again: a host with a path after it is not what RedactUrl writes");
  Check(reduced("urnetwork://auth [redacted five chars]") == "urnetwork://auth [redacted 16 chars] [redacted five chars]",
        "again: a length that is not digits");
  Check(reduced("urnetwork://auth [redacted 12") == "urnetwork://auth [redacted 16 chars] [redacted 12",
        "again: words cut short");
  Check(reduced("urnetwork://auth [redacted  12 chars]") == "urnetwork://auth [redacted 16 chars] [redacted  12 chars]",
        "again: a second space");
  Check(reduced("urnetwork://auth [redacted 12 chars").find("[redacted 16 chars]") != std::string::npos,
        "again: no closing, so it is no redaction of ours");
  Check(reduced("urnetwork://a.very.long.host.name.that.runs.past.thirty.two [redacted 5 chars]").find("past.thirty") == std::string::npos,
        "again: a host too long to be echoed is not one RedactUrl wrote");
  Check(reduced("urnetwork://auth: [redacted 5 chars]").find("urnetwork://auth: [redacted 5") == std::string::npos,
        "again: a host with a colon is not one RedactUrl wrote");
  // its scheme is a plain token too: "urnetwork:x" is not one (it holds a colon), so "urnetwork:x://y" is no link of ours
  Check(reduced("urnetwork:x://y [redacted 5 chars]") == "[redacted 15 chars] [redacted 5 chars]",
        "again: a scheme RedactUrl would not echo is not one it wrote");
  // and the words are exactly " [redacted ", a length and " chars]": one character off in any place of either is no
  // redaction of ours (the rest is right each time, so only that place can tell them apart; the first place is a tab, which
  // ends a link as a space does)
  const std::string opening = " [redacted ";
  for (std::size_t at = 0; at < opening.size(); ++at) {
    std::string off = opening;
    off[at] = at == 0 ? '\t' : '!';
    const std::string words = "urnetwork://auth" + off + "5 chars]";
    Check(reduced(words) == "urnetwork://auth [redacted 16 chars]" + words.substr(16),
          "again: words with place " + std::to_string(at) + " of ' [redacted ' off are no redaction of ours");
  }
  const std::string closing = " chars]";
  for (std::size_t at = 0; at < closing.size(); ++at) {
    std::string off = closing;
    off[at] = '!';
    const std::string words = "urnetwork://auth [redacted 5" + off;
    Check(reduced(words) == "urnetwork://auth [redacted 16 chars]" + words.substr(16),
          "again: words with place " + std::to_string(at) + " of ' chars]' off are no redaction of ours");
  }
  // and redacting what was redacted is a no-op for everything the other cases make
  for (const std::string& text : {link + " x", "a " + link + "\r\n", std::string("x UrNetwork://Auth?state=SECRETSTATE y"),
                                  std::string("urnetwork:auth?z=SECRETZ"), std::string("no link at all"),
                                  std::string("a urnetwork://oauth/google?id_token=SECRETONE b \"urnetwork://auth?state=SECRETTWO\" c")}) {
    const std::string once = authlink::RedactLogText(text);
    Check(authlink::RedactLogText(once) == once, "again: reducing what was reduced changes nothing");
  }
}

// The edges of what a redaction echoes (a short, plain scheme and host) and of what it takes for its own work, to the
// character and to the byte, and of two comparisons the routing and the search of a command line make. Each case below
// changes one thing between two links, so the check that tells them apart is the only one that can fail.
void TestTheEdgesOfWhatTheLogMayEchoAndKeep() {
  // the routing compares the whole prefix, the first letter and the last included, and never reads past the view it was given
  Check(!authlink::IsAuthHost("xrnetwork://auth?x=1") && !authlink::IsAuthHost("Xrnetwork://auth"),
        "edges: a scheme that differs from the auth host's in its first letter only is not the auth host");
  Check(!authlink::IsAuthHost("urnetwork://autx?x=1") && !authlink::IsAuthHost("urnetwork://autx"),
        "edges: a host that differs in its last letter only is not the auth host");
  const std::string backing = "urnetwork://auxx?";  // the view stops at "urnetwork://au"; the storage goes on with what a router would look for
  Check(!authlink::IsAuthHost(std::string_view(backing).substr(0, 14)),
        "edges: a view shorter than the host is not the host, whatever is stored after it");
  Check(authlink::LinkInCommandLine(L"app.exe xrnetwork://auth?x=1").empty() &&
            authlink::RedactCommandLine(L"xrnetwork:auth?s=SECRET") == L"xrnetwork:auth?s=SECRET",
        "edges: a scheme that differs in its first letter only is no link in a command line, and is left as it is");

  // the sealed values' own caps
  const auto sealedWith = [](const std::string& epk, const std::string& nonce) {
    return kAuth + "?state=" + kState + "&epk=" + epk + "&n=" + nonce + "&d=" + kData;
  };
  Check(authlink::Parse(sealedWith(std::string(64, 'E'), kNonce)).kind == Kind::Sealed &&
            authlink::Parse(sealedWith(kEpk, std::string(64, 'N'))).kind == Kind::Sealed,
        "edges: a public key and a nonce of 64 characters are read");
  ExpectReject(sealedWith(std::string(65, 'E'), kNonce), Reject::BadValue, "edges: a public key of 65 characters is not");
  ExpectReject(sealedWith(kEpk, std::string(65, 'N')), Reject::BadValue, "edges: a nonce of 65 characters is not");

  // a scheme is echoed from one to sixteen plain letters, a host up to thirty-two; past that, or with a '#' where the host
  // ends, or with a character no plain token has, it is not
  const auto letters = [](std::size_t length, char c) { return std::string(length, c); };
  Check(authlink::RedactUrl("a://h/x") == "a://h [redacted 7 chars]", "edges: a scheme of one letter is echoed");
  Check(authlink::RedactUrl(letters(16, 'a') + "://h/x") == letters(16, 'a') + "://h [redacted " + std::to_string(16 + 6) + " chars]",
        "edges: a scheme of sixteen letters is echoed");
  Check(authlink::RedactUrl(letters(17, 'a') + "://h/x") == "[redacted " + std::to_string(17 + 6) + " chars]",
        "edges: a scheme of seventeen letters is not");
  Check(authlink::RedactUrl("://h/x") == "[redacted 6 chars]", "edges: no scheme at all is not echoed either");
  Check(authlink::RedactUrl("urnetwork://" + letters(32, 'h') + "/x") ==
            "urnetwork://" + letters(32, 'h') + " [redacted " + std::to_string(12 + 32 + 2) + " chars]",
        "edges: a host of thirty-two letters is echoed");
  Check(authlink::RedactUrl("urnetwork://" + letters(33, 'h') + "/x") ==
            "urnetwork://* [redacted " + std::to_string(12 + 33 + 2) + " chars]",
        "edges: a host of thirty-three letters is not");
  Check(authlink::RedactUrl("urnetwork://auth#fragment") == "urnetwork://auth [redacted 25 chars]",
        "edges: a host that a '#' ends is echoed");
  Check(authlink::RedactUrl("urnetwork://user:SECRETPW@host/?x=1") == "urnetwork://* [redacted 35 chars]",
        "edges: a host with userinfo in front of it is a '*', exactly");
  const std::string plain = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._+-";
  for (int byte = 1; byte < 256; ++byte) {
    const char c = static_cast<char>(byte);
    const bool isPlain = plain.find(c) != std::string::npos;
    const std::string called = "#" + std::to_string(byte);
    if (std::string_view("/?#:@").find(c) == std::string_view::npos) {
      const std::string hostUrl = std::string("urnetwork://a") + c + "b";
      const std::string wantHost = (isPlain ? hostUrl : std::string("urnetwork://*")) + " [redacted " + std::to_string(hostUrl.size()) + " chars]";
      Check(authlink::RedactUrl(hostUrl) == wantHost, "edges: byte " + called + " in a host is " + (isPlain ? "plain" : "not plain"));
    }
    if (std::string_view(":/").find(c) == std::string_view::npos) {
      const std::string schemeUrl = std::string("a") + c + "b://h";
      const std::string wantScheme = (isPlain ? schemeUrl + " " : std::string()) + "[redacted " + std::to_string(schemeUrl.size()) + " chars]";
      Check(authlink::RedactUrl(schemeUrl) == wantScheme, "edges: byte " + called + " in a scheme is " + (isPlain ? "plain" : "not plain"));
    }
  }

  // what is taken for a redaction already made: a host of thirty-two, the digits 0 to 9 and nothing beside them, at least one,
  // and as many as the window after a link holds (twenty-two)
  const auto again = [](const std::string& text) { return authlink::RedactLogText(text); };
  const std::string own32 = "urnetwork://" + letters(32, 'h') + " [redacted 99 chars]";
  Check(again(own32) == own32, "edges: a redaction whose host has thirty-two letters is its own");
  Check(again("urnetwork://" + letters(33, 'h') + " [redacted 99 chars]").find(letters(33, 'h')) == std::string::npos,
        "edges: words after a host of thirty-three letters are no redaction of ours");
  for (const std::string digits : {"0", "9", "09", "1234567890"}) {
    const std::string own = "urnetwork://auth [redacted " + digits + " chars]";
    Check(again(own) == own, "edges: " + digits + " is a length");
  }
  for (const std::string notDigits : {":5", "/5", "5:", "5/", "a", ""}) {
    const std::string words = "urnetwork://auth [redacted " + notDigits + " chars]";
    Check(again(words) == "urnetwork://auth [redacted 16 chars]" + words.substr(16),
          "edges: '" + notDigits + "' is no length: the link before it is reduced, and the words are kept");
  }
  const std::string own22 = "urnetwork://auth [redacted " + letters(22, '7') + " chars]";
  const std::string past22 = "urnetwork://auth [redacted " + letters(23, '7') + " chars]";
  Check(again(own22) == own22, "edges: a length of twenty-two digits is read in full");
  Check(again(past22) != past22, "edges: the window after a link holds twenty-two digits, and no more");
}

}  // namespace

int main() {
  TestTheParserAcceptsBothShapes();
  TestTheParserRefusesEverythingElse();
  TestAPercentSignIsRefusedEverywhere();
  TestEveryEscapeIsRefusedAsOne();
  TestOneHostileCharacterAnywhereMakesItNoLink();
  TestThePagesOwnAnswersAreRead();
  TestTheSpecsAlphabetsAreTheStandardOnes();
  TestEveryCharacterOfEachAlphabetIsRead();
  TestNoCharacterOutsideAnAlphabetIsRead();
  TestBase58LeavesOutTheLookalikes();
  TestTheCapsAreTheContractsOwnToTheByte();
  TestTheOtherContracts();
  TestRoutingSendsOnlyTheAuthHostToTheGate();
  TestTheTokensAreClosed();
  TestALinkInACommandLineIsFoundWhateverItsCase();
  TestRedaction();
  TestLoggedTextIsRedactedLineByLine();
  TestARedactionIsNotRedactedAgain();
  TestTheEdgesOfWhatTheLogMayEchoAndKeep();
  std::cout << (gCases - gFailures) << "/" << gCases << " auth link checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
