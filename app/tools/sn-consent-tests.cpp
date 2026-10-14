// Executable spec for Earnings' payout consent (App/SnConsentPresentation.h,
// v2/CONSENT.md §4 and §8): what the panel after settlement v2 shows for every
// state of the consent read and of a signing in flight (no consent, a consent,
// the consent a signing just made, which starts next epoch), that the coldkey
// the consent names is what the user typed and passes the ss58 rule before any
// request, which store sentence each refusal shows (an expired challenge, a
// signature that does not match, a session that is not the network owner's,
// and none for any other), and that what is submitted is the server's own
// message for the coldkey it names, never anything the wallet handed back
// instead. Run against the same header the app compiles, on any host with a
// C++20 compiler.
//
// The WinUI half (WalletPage's panel, its button, the coldkey dialog and the
// wallet hop) cannot be built off Windows at all; what is verified here is every
// decision it makes before it touches a XAML object.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App sn-consent-tests.cpp -o /tmp/sn-consent-tests && /tmp/sn-consent-tests
//
// With URNW_SN_CONSENT_TESTS_SDK the SDK's status and challenge are the
// generated header's own (urnet::SnNetworkConsentStatus and
// urnet::SnNetworkConsentChallengeResult, read from the json the api answers
// with through the header's conversions), which is what WalletPage and SdkHost
// instantiate the boundary templates with, and the refusal codes are compared
// with the header's. The header needs nlohmann/json; both are system includes
// because the generated code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_SN_CONSENT_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> sn-consent-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "SnConsentPresentation.h"

#if defined(URNW_SN_CONSENT_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw::snconsent;

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

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

#if defined(URNW_SN_CONSENT_TESTS_SDK)
using SdkStatus = urnet::SnNetworkConsentStatus;
using SdkChallenge = urnet::SnNetworkConsentChallengeResult;
#else
// The SDK's status and challenge, by the generated wrapper's names and types
// (v2/CONSENT.md §9: the json shapes' snake_case names).
struct SdkStatus {
  bool active{};
  std::string coldkey_ss58{};
  int64_t from_epoch{};
  int64_t through_epoch{};
  int64_t generation{};
};
struct SdkChallenge {
  std::string message{};
  std::string coldkey_ss58{};
  int64_t from_epoch{};
  int64_t through_epoch{};
  int64_t expires_at{};
};
#endif

constexpr const char* kColdkey = "5F3sa2TJAWMqDhXG6jhV4N8ko9SxwGy8TpaNS1repo5EYjQX";
constexpr const char* kOtherColdkey = "5GrwvaEF5zXb26Fz9rcQpDWS57CtERHpNehXCPcNoHGKutQY";
// the current epoch the examples are signed in: a consent starts at the next
constexpr int64_t kCurrentEpoch = 120;

// The server's network consent message, as it is shaped (protocol
// NetworkWalletMappingStatement.Message: the prefix, then the statement).
const std::string kMessage =
    "Approve URnetwork network wallet mapping\n"
    "{\"schema\":\"urnetwork-network-wallet-mapping-consent-v1\",\"scope\":\"network\","
    "\"domain\":{\"chain_id\":964,\"netuid\":25},\"from_epoch\":121,\"through_epoch\":65656}";

Status Consent(bool active, const std::string& coldkey, int64_t fromEpoch) {
  Status status;
  status.active = active;
  status.coldkeySs58 = coldkey;
  status.fromEpoch = fromEpoch;
  status.throughEpoch = fromEpoch + 65'535;
  return status;
}

Challenge IssuedChallenge() {
  Challenge challenge;
  challenge.message = kMessage;
  challenge.coldkeySs58 = kColdkey;
  challenge.fromEpoch = kCurrentEpoch + 1;
  challenge.throughEpoch = kCurrentEpoch + 1 + 65'535;
  challenge.expiresAt = 1'791'849'900;
  return challenge;
}

void CheckView(const PanelView& want, const PanelView& got, const std::string& what) {
  Check(want.shown == got.shown, what + ": shown");
  Check(want.loading == got.loading, what + ": loading");
  Check(want.failed == got.failed, what + ": failed");
  Check(want.line == got.line, what + ": line");
  CheckEq(want.coldkeySs58, got.coldkeySs58, what + ": coldkey");
  CheckEq(want.noteKey, got.noteKey, what + ": note");
  CheckEq(want.buttonKey, got.buttonKey, what + ": button");
  Check(want.buttonEnabled == got.buttonEnabled, what + ": button enabled");
}

}  // namespace

int main() {
  const Status none = Consent(false, std::string(), 0);
  const Status active = Consent(true, kColdkey, kCurrentEpoch - 3);

  {
    TEST_CASE("the panel: hidden until the consent is read, in every state");
    for (const bool signing : {false, true}) {
      CheckView({}, PanelFor(true, Fetch::Idle, active, signing), "with a consent");
      CheckView({}, PanelFor(true, Fetch::Idle, none, signing), "with none");
      CheckView({}, PanelFor(true, Fetch::Idle, std::nullopt, signing), "unread");
    }
  }
  {
    TEST_CASE("the panel: Loading until the first read answers");
    PanelView want;
    want.shown = true;
    want.loading = true;
    CheckView(want, PanelFor(true, Fetch::Loading, std::nullopt, false), "loading");
  }
  {
    TEST_CASE("the panel: no consent, then Sign with wallet");
    PanelView want;
    want.shown = true;
    want.line = Line::None;
    want.noteKey = "sn_consent_body";
    want.buttonKey = "sn_consent_sign";
    want.buttonEnabled = true;
    CheckView(want, PanelFor(true, Fetch::Ready, none, false), "none");
    // a consent whose through epoch passed pays nothing: none, as with no consent
    CheckView(want, PanelFor(true, Fetch::Ready, Consent(false, kColdkey, 1), false),
              "a consent that ended");
  }
  {
    TEST_CASE("the panel: a consent says whose coldkey it pays, then Change wallet");
    PanelView want;
    want.shown = true;
    want.line = Line::Active;
    want.coldkeySs58 = kColdkey;
    want.noteKey = "sn_consent_change_note";
    want.buttonKey = "sn_consent_change";
    want.buttonEnabled = true;
    CheckView(want, PanelFor(true, Fetch::Ready, active, false), "active");
  }
  {
    TEST_CASE("the panel: right after a signing the consent is active, starting next epoch");
    // the read before the signing answered none; the read after the submit
    // answers the new consent, which the server took for the next epoch only
    const Status signedNow = Consent(true, kColdkey, kCurrentEpoch + 1);
    Check(signedNow.fromEpoch > kCurrentEpoch, "the example starts next epoch");
    PanelView want;
    want.shown = true;
    want.line = Line::Active;
    want.coldkeySs58 = kColdkey;
    want.noteKey = "sn_consent_change_note";  // "A new wallet takes effect from the next epoch."
    want.buttonKey = "sn_consent_change";
    want.buttonEnabled = true;
    CheckView(want, PanelFor(true, Fetch::Ready, signedNow, false), "after the signing");
    // while that read is out, the read before it stays on screen
    const PanelView reading = PanelFor(true, Fetch::Loading, none, false);
    Check(reading.loading && reading.line == Line::None, "the read before it stays");
  }
  {
    TEST_CASE("the panel: Waiting for the wallet while a signing is out");
    PanelView want;
    want.shown = true;
    want.line = Line::None;
    want.noteKey = "sn_consent_signing";
    want.buttonKey = "sn_consent_sign";
    want.buttonEnabled = false;
    CheckView(want, PanelFor(true, Fetch::Ready, none, true), "signing over none");
    want.line = Line::Active;
    want.coldkeySs58 = kColdkey;
    want.buttonKey = "sn_consent_change";
    CheckView(want, PanelFor(true, Fetch::Ready, active, true), "changing");
    // a read that failed meanwhile does not hide the wait
    PanelView failed;
    failed.shown = true;
    failed.failed = true;
    failed.noteKey = "sn_consent_signing";
    failed.buttonKey = "sn_consent_sign";
    CheckView(failed, PanelFor(true, Fetch::Failed, std::nullopt, true),
              "signing over a failed read");
  }
  {
    TEST_CASE("the panel: the last read's consent stays while the next read is out");
    PanelView want;
    want.shown = true;
    want.loading = true;
    want.line = Line::Active;
    want.coldkeySs58 = kColdkey;
    want.noteKey = "sn_consent_change_note";
    want.buttonKey = "sn_consent_change";
    want.buttonEnabled = true;
    CheckView(want, PanelFor(true, Fetch::Loading, active, false), "reloading");
  }
  {
    TEST_CASE("the panel: a failed read shows no consent and no button");
    PanelView want;
    want.shown = true;
    want.failed = true;
    CheckView(want, PanelFor(true, Fetch::Failed, std::nullopt, false), "failed");
    // never the consent of a read before it, nor a "none" it cannot know
    CheckView(want, PanelFor(true, Fetch::Failed, active, false), "failed over a consent");
    CheckView(want, PanelFor(true, Fetch::Failed, none, false), "failed over none");
  }
  {
    TEST_CASE("the panel: without a session to sign with, the button waits");
    Check(!PanelFor(false, Fetch::Ready, none, false).buttonEnabled, "Sign with wallet");
    Check(!PanelFor(false, Fetch::Ready, active, false).buttonEnabled, "Change wallet");
    CheckEq("sn_consent_sign", PanelFor(false, Fetch::Ready, none, false).buttonKey,
            "still shown");
  }
  {
    TEST_CASE("the coldkey: what the user typed, trimmed, checked before any request");
    std::vector<std::string> checked;
    const auto valid = [&checked](const std::string& address) {
      checked.push_back(address);
      return address == kColdkey || address == kOtherColdkey;
    };
    const auto typed = ColdkeyFrom(std::string("  ") + kColdkey + "\r\n", valid);
    Check(typed.has_value() && *typed == kColdkey, "the typed coldkey, trimmed");
    Check(checked == std::vector<std::string>{kColdkey}, "the ss58 rule sees the trimmed text");
    // another account than the one that signs in is the user's choice to make
    Check(ColdkeyFrom(kOtherColdkey, valid) == std::optional<std::string>(kOtherColdkey),
          "any valid coldkey");
  }
  {
    TEST_CASE("the coldkey: nothing, or an address the ss58 rule refuses, asks for nothing");
    int calls = 0;
    const auto valid = [&calls](const std::string& address) {
      ++calls;
      return address == kColdkey;
    };
    Check(!ColdkeyFrom("", valid), "empty");
    Check(!ColdkeyFrom(" \t\n", valid), "only whitespace");
    Check(calls == 0, "nothing typed is not checked");
    Check(!ColdkeyFrom("5F3sa2TJAWMqDhXG6jhV4N8ko9SxwGy8TpaNS1repo5EYjQx", valid),
          "a bad checksum");
    Check(!ColdkeyFrom("0x8b3f9a1e2c4d5f6071829a3b4c5d6e7f8091a2b3", valid), "an EVM address");
    Check(calls == 2, "each typed address is checked");
  }
  {
    TEST_CASE("refusals: an expired challenge says so");
    CheckEq("sn_consent_expired", RefusalKey("challenge_expired"), "the session's code");
    CheckEq("sn_consent_expired",
            RefusalKey("challenge_expired: network wallet mapping original challenge is expired"),
            "the SDK's error");
  }
  {
    TEST_CASE("refusals: a signature that does not match says so");
    CheckEq("sn_consent_failed", RefusalKey("signature_mismatch"), "SnSetWalletResult's code");
    CheckEq("sn_consent_failed", RefusalKey("signature_mismatch: another account signed"),
            "an error that starts with it");
  }
  {
    TEST_CASE("refusals: a session that is not the network owner's says so");
    CheckEq("sn_consent_owner_only", RefusalKey("network_owner_only"), "the code");
    CheckEq("sn_consent_owner_only", RefusalKey("network_owner_only: no network credential"),
            "the SDK's error");
  }
  {
    TEST_CASE("refusals: any other goes to the page's existing error surface");
    for (const char* other : {"", "invalid_ss58_address", "wallet_blocked: this wallet",
                              "403 Forbidden", "wallet_error", "address_mismatch",
                              "challenge_expired_soon", "network_owner_onlyx: x",
                              "the request failed: challenge_expired"}) {
      CheckEq("", RefusalKey(other), std::string("\"") + other + "\"");
    }
  }
  {
    TEST_CASE("the submission: the server's message for its coldkey, with the signature");
    const Challenge challenge = IssuedChallenge();
    const auto submission = SubmissionFor(challenge, kColdkey, kMessage, "0xsig");
    Check(submission.has_value(), "a signature over the issued message");
    if (submission) {
      CheckEq(kColdkey, submission->coldkeySs58, "coldkey");
      CheckEq(kMessage, submission->message, "message");
      CheckEq("0xsig", submission->signatureHex, "signature");
    }
  }
  {
    TEST_CASE("the submission: never another message");
    const Challenge challenge = IssuedChallenge();
    Check(!SubmissionFor(challenge, kColdkey, "Sign in to URnetwork\nChallenge: x\nTimestamp: 1",
                         "0xsig"),
          "a login challenge");
    Check(!SubmissionFor(challenge, kColdkey, kMessage + " ", "0xsig"), "a trailing space");
    std::string crlf = kMessage;
    crlf.replace(crlf.find('\n'), 1, "\r\n");
    Check(!SubmissionFor(challenge, kColdkey, crlf, "0xsig"), "a CRLF spelling");
    Check(!SubmissionFor(challenge, kColdkey, "<Bytes>" + kMessage + "</Bytes>", "0xsig"),
          "the wallet's wrapped bytes");
    Check(!SubmissionFor(challenge, kColdkey, "", "0xsig"), "nothing");
  }
  {
    TEST_CASE("the submission: never another account");
    const Challenge challenge = IssuedChallenge();
    Check(!SubmissionFor(challenge, kOtherColdkey, kMessage, "0xsig"), "another coldkey");
    Check(!SubmissionFor(challenge, "", kMessage, "0xsig"), "no address");
  }
  {
    TEST_CASE("the submission: nothing without a signature or a challenge");
    Check(!SubmissionFor(IssuedChallenge(), kColdkey, kMessage, ""), "no signature");
    const Challenge empty;
    Check(!SubmissionFor(empty, "", "", "0xsig"), "no challenge");
    Challenge unbound = IssuedChallenge();
    unbound.coldkeySs58.clear();
    Check(!SubmissionFor(unbound, "", kMessage, "0xsig"), "a challenge for no coldkey");
  }
  {
    TEST_CASE("the SDK's boundary: the status and the challenge as plain views");
    SdkStatus sdkStatus;
    sdkStatus.active = true;
    sdkStatus.coldkey_ss58 = kColdkey;
    sdkStatus.from_epoch = kCurrentEpoch + 1;
    sdkStatus.through_epoch = kCurrentEpoch + 1 + 65'535;
    sdkStatus.generation = 0;  // the server does not say yet (§8.2)
    Check(StatusFrom(sdkStatus) == Consent(true, kColdkey, kCurrentEpoch + 1), "the status");
    const SdkStatus sdkNone{};
    Check(StatusFrom(sdkNone) == Status{}, "no consent");

    SdkChallenge sdkChallenge;
    sdkChallenge.message = kMessage;
    sdkChallenge.coldkey_ss58 = kColdkey;
    sdkChallenge.from_epoch = kCurrentEpoch + 1;
    sdkChallenge.through_epoch = kCurrentEpoch + 1 + 65'535;
    sdkChallenge.expires_at = 1'791'849'900;
    Check(ChallengeFrom(sdkChallenge) == IssuedChallenge(), "the challenge, byte for byte");
#if defined(URNW_SN_CONSENT_TESTS_SDK)
    // what the api's callbacks parse: the json shapes of v2/CONSENT.md §9,
    // through the header's own conversions
    const auto parsedStatus =
        nlohmann::json::parse(R"({"active":true,"coldkey_ss58":")" + std::string(kColdkey) +
                              R"(","from_epoch":121,"through_epoch":65656,"generation":0})")
            .get<urnet::SnNetworkConsentStatus>();
    Check(StatusFrom(parsedStatus) == Consent(true, kColdkey, kCurrentEpoch + 1),
          "the header's status");
    const nlohmann::json challengeJson = {{"message", kMessage},
                                          {"coldkey_ss58", kColdkey},
                                          {"from_epoch", kCurrentEpoch + 1},
                                          {"through_epoch", kCurrentEpoch + 1 + 65'535},
                                          {"expires_at", 1'791'849'900}};
    const auto parsedChallenge = challengeJson.get<urnet::SnNetworkConsentChallengeResult>();
    Check(ChallengeFrom(parsedChallenge) == IssuedChallenge(), "the header's challenge");
    // and back, as the session's setNetworkConsentChallenge sends it: the
    // message byte for byte
    CheckEq(kMessage, nlohmann::json(parsedChallenge).at("message").get<std::string>(),
            "the challenge the session takes");
    // the codes the refusals carry are the header's
    CheckEq(urnet::SnErrorCodeChallengeExpired, std::string(kCodeChallengeExpired),
            "challenge_expired");
    CheckEq(urnet::SnErrorCodeSignatureMismatch, std::string(kCodeSignatureMismatch),
            "signature_mismatch");
    CheckEq(urnet::SnErrorCodeNetworkOwnerOnly, std::string(kCodeNetworkOwnerOnly),
            "network_owner_only");
#endif
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
