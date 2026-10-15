// Executable spec for Earnings › Top 200 › Sign fleet binding (App/Top200Binding.h):
// which pasted text is a fleet binding of this device, the payload it signs
// into, the member entry, and what a urnetwork://top200/sign link pre-fills.
// The vector is sn's fleet_sign_test.go fixture (client seed 0x01..0x20), which
// mmm's tests/top200-fleet-binding.test.mjs uses too; the signer is a fake that
// returns the fixture's signature.
//
//   c++ -std=c++20 -I ../src/App -isystem <nlohmann include> top200-sign-tests.cpp -o /tmp/top200-sign-tests
//   /tmp/top200-sign-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <stdexcept>
#include <string>

#include "Top200Binding.h"

using namespace urnw::top200sign;

namespace {

int gFailures = 0;
int gCases = 0;

void Expect(bool ok, std::string const& what) {
  ++gCases;
  if (!ok) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(std::string const& want, std::string const& got, std::string const& what) {
  ++gCases;
  if (want != got) {
    ++gFailures;
    std::cout << "  FAIL " << what << ": want \"" << want << "\", got \"" << got << "\"\n";
  }
}

char const* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::Empty:
      return "Empty";
    case Verdict::Invalid:
      return "Invalid";
    case Verdict::NoKey:
      return "NoKey";
    case Verdict::OtherDevice:
      return "OtherDevice";
    case Verdict::Valid:
      return "Valid";
  }
  return "?";
}

void CheckVerdict(Verdict want, std::string const& text, std::string const& id,
                  std::string const& key, std::string const& what) {
  CheckEq(VerdictName(want), VerdictName(Validate(text, id, key).verdict), what);
}

const std::string kClientId = "019b2e10-5a7c-4d3e-8f01-23456789abcd";
const std::string kClientKey = "0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664";
const std::string kSignature =
    "0x8a3e5b990f1a280f0eef9455c81dfbfd251a5ac5a9bc021770cf0a4dee1b4aafab04762d6ca8b587c1e04642d46cff8"
    "7844b89aefcaddf26700fdbc849efbc0a";
const std::string kBinding =
    R"({"chain_id":945,"netuid":25,"coordinator":"0xc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3",)"
    R"("fleet_id":"0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef",)"
    R"("hotkey":"0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55c",)"
    R"("client_id":"019b2e10-5a7c-4d3e-8f01-23456789abcd",)"
    R"("client_key":"0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664",)"
    R"("generation":3,"valid_from_epoch":100,"valid_to_epoch":200,)"
    R"("commitment_hash":"0x9e1d4fa4a979f45cf003f26d826837250b08b45b8e896f98316cfc60005d3471"})";
// sn's fleetSignGolden payload, byte for byte (the spec's test vector)
const std::string kPayload =
    R"({"binding":{"chain_id":945,"netuid":25,"coordinator":"0xc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3",)"
    R"("fleet_id":"0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef",)"
    R"("hotkey":"0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55c",)"
    R"("client_id":"019b2e10-5a7c-4d3e-8f01-23456789abcd",)"
    R"("client_key":"0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664",)"
    R"("generation":3,"valid_from_epoch":100,"valid_to_epoch":200,)"
    R"("commitment_hash":"0x9e1d4fa4a979f45cf003f26d826837250b08b45b8e896f98316cfc60005d3471"},)"
    R"("client_signature":"0x8a3e5b990f1a280f0eef9455c81dfbfd251a5ac5a9bc021770cf0a4dee1b4aafab04762d6ca8b587c1e04642d46cff87844b89aefcaddf26700fdbc849efbc0a"})";
// the binding as the page's app link carries it (python base64.urlsafe_b64encode, '=' stripped)
const std::string kBindingBase64Url =
    "eyJjaGFpbl9pZCI6OTQ1LCJuZXR1aWQiOjI1LCJjb29yZGluYXRvciI6IjB4YzBjMWMyYzNjNGM1YzZjN2M4YzljYWNiY2NjZG"
    "NlY2ZkMGQxZDJkMyIsImZsZWV0X2lkIjoiMHhmMGYxZjJmM2Y0ZjVmNmY3ZjhmOWZhZmJmY2ZkZmVmZmUwZTFlMmUzZTRlNWU2"
    "ZTdlOGU5ZWFlYmVjZWRlZWVmIiwiaG90a2V5IjoiMHgwYTFjNzdlZGI0ODUxOTA2OWJiYjE1NGJmMDg4YzdmMTc2Nzg4MzlkZT"
    "Q1OGUwNjc5MzcwYWU5ODlhZjNmNTVjIiwiY2xpZW50X2lkIjoiMDE5YjJlMTAtNWE3Yy00ZDNlLThmMDEtMjM0NTY3ODlhYmNk"
    "IiwiY2xpZW50X2tleSI6IjB4NzliNTU2MmU4ZmU2NTRmOTQwNzhiMTEyZThhOThiYTc5MDFmODUzYWU2OTViZWQ3ZTBlMzkxMG"
    "JhZDA0OTY2NCIsImdlbmVyYXRpb24iOjMsInZhbGlkX2Zyb21fZXBvY2giOjEwMCwidmFsaWRfdG9fZXBvY2giOjIwMCwiY29t"
    "bWl0bWVudF9oYXNoIjoiMHg5ZTFkNGZhNGE5NzlmNDVjZjAwM2YyNmQ4MjY4MzcyNTBiMDhiNDViOGU4OTZmOTgzMTZjZmM2MD"
    "AwNWQzNDcxIn0";

// kBinding with the first `from` replaced by `to`
std::string Replace(std::string const& from, std::string const& to) {
  std::string out = kBinding;
  const size_t at = out.find(from);
  if (at == std::string::npos) {
    std::cout << "  FAIL the fixture has no " << from << "\n";
    ++gFailures;
    return out;
  }
  out.replace(at, from.size(), to);
  return out;
}

}  // namespace

int main() {
  // ---- the vector ----------------------------------------------------------
  {
    const auto check = Validate(kBinding, kClientId, kClientKey);
    CheckEq("Valid", VerdictName(check.verdict), "the vector's binding is this device's");
    Expect(check.binding.has_value(), "a valid binding carries its fields");
    if (check.binding) {
      Expect(check.binding->chainId == 945 && check.binding->netuid == 25, "chain and subnet");
      Expect(check.binding->validFromEpoch == 100 && check.binding->validToEpoch == 200, "epochs");
      Expect(check.binding->generation == 3, "generation");
      CheckEq("0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef", check.binding->fleetId,
              "fleet id");
      CheckEq("0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55c", check.binding->hotkey,
              "hotkey");
    }
    int calls = 0;
    std::string signedText;
    const Signed result = SignChecked(check, [&](std::string const& text) {
      ++calls;
      signedText = text;
      return kSignature;
    });
    Expect(result.ok, "the fake signer's signature makes a payload");
    CheckEq(kPayload, result.payload, "the payload is sn's golden, byte for byte");
    CheckEq(kBinding, signedText, "the signer gets the binding text");
    Expect(calls == 1, "the signer runs once");
    CheckEq(kPayload, Payload(kBinding, kSignature), "Payload concatenates");
  }

  // ---- what is pasted around it ----------------------------------------------
  {
    const std::string padded = "\r\n  \t" + kBinding + "\n\n ";
    const auto check = Validate(padded, kClientId, kClientKey);
    CheckEq("Valid", VerdictName(check.verdict), "surrounding whitespace is trimmed");
    CheckEq(kBinding, check.trimmed, "the trimmed text is the binding");
    const Signed result = SignChecked(check, [](std::string const&) { return kSignature; });
    CheckEq(kPayload, result.payload, "the payload wraps the trimmed text");

    // the payload keeps the binding's own spacing: it is never re-serialized
    const std::string spaced = Replace(R"("netuid":25,)", R"("netuid": 25, )");
    const auto spacedCheck = Validate(spaced, kClientId, kClientKey);
    CheckEq("Valid", VerdictName(spacedCheck.verdict), "inner whitespace is a valid binding");
    const Signed spacedResult = SignChecked(spacedCheck, [](std::string const&) { return kSignature; });
    Expect(spacedResult.payload.find(R"("netuid": 25, )") != std::string::npos,
          "the payload keeps the binding's own spacing");
  }
  CheckVerdict(Verdict::Empty, "", kClientId, kClientKey, "an empty box");
  CheckVerdict(Verdict::Empty, " \r\n\t", kClientId, kClientKey, "a blank box");
  CheckVerdict(Verdict::Invalid, kPayload, kClientId, kClientKey,
               "a {binding, client_signature} payload is not a binding");
  CheckVerdict(Verdict::Invalid, kBinding + std::string(kMaxBindingTextLength, ' ') + "x", kClientId, kClientKey,
               "text past the length cap is not parsed");
  CheckVerdict(Verdict::Valid, kBinding + std::string(kMaxBindingTextLength, ' '), kClientId, kClientKey,
               "the cap counts the trimmed text");
  CheckVerdict(Verdict::Invalid, "\xEF\xBB\xBF" + kBinding, kClientId, kClientKey,
               "a UTF-8 byte order mark before the object (Go refuses it)");
  CheckVerdict(Verdict::Invalid, "\xC2\xA0" + kBinding, kClientId, kClientKey,
               "a non-ASCII space before the object");
  CheckVerdict(Verdict::Invalid, "not json", kClientId, kClientKey, "not JSON");
  CheckVerdict(Verdict::Invalid, "[" + kBinding + "]", kClientId, kClientKey, "an array");
  CheckVerdict(Verdict::Invalid, kBinding + "x", kClientId, kClientKey, "trailing garbage");
  CheckVerdict(Verdict::Invalid, "\"" + kBinding + "\"", kClientId, kClientKey, "a JSON string");

  // ---- this device or another ----------------------------------------------
  CheckVerdict(Verdict::OtherDevice, kBinding, "019b2e10-5a7c-4d3e-8f01-23456789abce", kClientKey,
               "another client id");
  CheckVerdict(Verdict::OtherDevice, kBinding, kClientId,
               "0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049665",
               "another client key (the SDK would sign it)");
  CheckVerdict(Verdict::OtherDevice,
               Replace("0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664",
                       "0x0000000000000000000000000000000000000000000000000000000000000001"),
               kClientId, kClientKey, "the binding names another key");
  CheckVerdict(Verdict::OtherDevice, kBinding, "", kClientKey, "no device client id");
  CheckVerdict(Verdict::NoKey, kBinding, kClientId, "", "no client key: the service is down");
  CheckVerdict(Verdict::Invalid, kPayload, kClientId, "", "a payload without a key is still invalid");
  CheckVerdict(Verdict::Valid,
               Replace("\"019b2e10-5a7c-4d3e-8f01-23456789abcd\"", "\"0x019B2E105A7C4D3E8F0123456789ABCD\""),
               kClientId, kClientKey, "client_id as 0x + 32 hex, either case");
  CheckVerdict(Verdict::Valid, kBinding, "019B2E10-5A7C-4D3E-8F01-23456789ABCD", kClientKey,
               "the device id in upper case");
  CheckVerdict(Verdict::Valid,
               Replace("0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664",
                       "0x79B5562E8FE654F94078B112E8A98BA7901F853AE695BED7E0E3910BAD049664"),
               kClientId, kClientKey, "client_key hex in upper case");
  {
    int calls = 0;
    for (auto const& check : {Validate(kBinding, kClientId, ""),
                              Validate(kBinding, "019b2e10-5a7c-4d3e-8f01-23456789abce", kClientKey),
                              Validate(kPayload, kClientId, kClientKey), Validate("", kClientId, kClientKey)}) {
      const Signed result = SignChecked(check, [&](std::string const&) {
        ++calls;
        return kSignature;
      });
      Expect(!result.ok && result.detail.empty() && result.payload.empty(),
            std::string("nothing is signed for ") + VerdictName(check.verdict));
    }
    Expect(calls == 0, "the signer is never called for a binding that is not Valid");
  }

  // ---- fields ----------------------------------------------------------------
  CheckVerdict(Verdict::Invalid, Replace(R"("generation":3,)", ""), kClientId, kClientKey, "a missing field");
  CheckVerdict(Verdict::Invalid, Replace(R"("commitment_hash")", R"("commitment")"), kClientId, kClientKey,
               "a misnamed field");
  CheckVerdict(Verdict::Invalid, Replace(R"("generation":3,)", R"("generation":3,"extra":1,)"), kClientId,
               kClientKey, "a twelfth field");
  CheckVerdict(Verdict::Invalid, Replace(R"("netuid":25,)", R"("netuid":25,"netuid":26,)"), kClientId,
               kClientKey, "a field named twice");
  // the SDK's Go decoder matches keys case-insensitively and lets a later one
  // win: a case variant could show one hotkey here and sign another
  CheckVerdict(Verdict::Invalid,
               Replace(R"("generation":3,)",
                       R"("generation":3,"HOTKEY":"0x1111111111111111111111111111111111111111111111111111111111111111",)"),
               kClientId, kClientKey, "a case variant of a field beside it");
  CheckVerdict(Verdict::Invalid, Replace(R"("hotkey":)", R"("Hotkey":)"), kClientId, kClientKey,
               "a field spelled in another case");
  CheckVerdict(Verdict::Invalid, Replace(R"("fleet_id":)", R"("fleet_id ":)"), kClientId, kClientKey,
               "a field spelled with a space");
  CheckVerdict(Verdict::Invalid, Replace("0xc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3", "0xc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2"),
               kClientId, kClientKey, "a coordinator of 19 bytes");
  CheckVerdict(Verdict::Invalid,
               Replace("0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeef",
                       "0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeffe0e1e2e3e4e5e6e7e8e9eaebecedeeefaa"),
               kClientId, kClientKey, "a fleet id of 33 bytes");
  CheckVerdict(Verdict::Invalid,
               Replace("0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55c",
                       "0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55g"),
               kClientId, kClientKey, "a hotkey with a non-hex digit");
  CheckVerdict(Verdict::Invalid,
               Replace("0x9e1d4fa4a979f45cf003f26d826837250b08b45b8e896f98316cfc60005d3471",
                       "9e1d4fa4a979f45cf003f26d826837250b08b45b8e896f98316cfc60005d347100"),
               kClientId, kClientKey, "a commitment hash without 0x");
  CheckVerdict(Verdict::Invalid, Replace("019b2e10-5a7c-4d3e-8f01-23456789abcd", "019b2e105a7c4d3e8f0123456789abcd"),
               kClientId, kClientKey, "a client id that is neither a uuid nor 0x hex");
  CheckVerdict(Verdict::Invalid, Replace(R"("valid_from_epoch":100,"valid_to_epoch":200)",
                                         R"("valid_from_epoch":200,"valid_to_epoch":100)"),
               kClientId, kClientKey, "valid_to_epoch before valid_from_epoch");
  CheckVerdict(Verdict::Valid, Replace(R"("valid_to_epoch":200)", R"("valid_to_epoch":100)"), kClientId, kClientKey,
               "a one-epoch window");
  CheckVerdict(Verdict::Invalid, Replace(R"("chain_id":945)", R"("chain_id":0)"), kClientId, kClientKey,
               "chain id 0");
  CheckVerdict(Verdict::Invalid, Replace(R"("generation":3)", R"("generation":0)"), kClientId, kClientKey,
               "generation 0");
  CheckVerdict(Verdict::Invalid, Replace(R"("netuid":25)", R"("netuid":65536)"), kClientId, kClientKey,
               "netuid past 65535");
  CheckVerdict(Verdict::Valid, Replace(R"("netuid":25)", R"("netuid":0)"), kClientId, kClientKey, "netuid 0");
  CheckVerdict(Verdict::Invalid, Replace(R"("valid_from_epoch":100)", R"("valid_from_epoch":-1)"), kClientId,
               kClientKey, "a negative epoch");
  CheckVerdict(Verdict::Invalid, Replace(R"("chain_id":945)", R"("chain_id":945.0)"), kClientId, kClientKey,
               "a float chain id");
  CheckVerdict(Verdict::Invalid, Replace(R"("generation":3)", R"("generation":[3])"), kClientId, kClientKey,
               "a generation in an array");
  CheckVerdict(Verdict::Invalid, Replace(R"("chain_id":945)", R"("chain_id":"945")"), kClientId, kClientKey,
               "a chain id in a string");
  CheckVerdict(Verdict::Invalid, Replace(R"("hotkey":"0x0a1c77edb48519069bbb154bf088c7f17678839de458e0679370ae989af3f55c")",
                                         R"("hotkey":12345)"), kClientId, kClientKey,
               "a hotkey that is not a string");

  // ---- what the signer returns -----------------------------------------------
  {
    const auto check = Validate(kBinding, kClientId, kClientKey);
    const Signed empty = SignChecked(check, [](std::string const&) { return std::string(); });
    Expect(!empty.ok && empty.payload.empty(), "an empty signature makes no payload");
    CheckEq("empty signature", empty.detail, "an empty signature's detail");
    const Signed thrown = SignChecked(check, [](std::string const&) -> std::string {
      throw std::runtime_error("binding client_id is not this device");
    });
    Expect(!thrown.ok && thrown.payload.empty(), "a refusal makes no payload");
    CheckEq("binding client_id is not this device", thrown.detail, "a refusal's detail is the SDK's text");
    const Signed odd = SignChecked(check, [](std::string const&) { return std::string("0x1234\"}"); });
    Expect(!odd.ok && odd.payload.empty(), "a malformed signature makes no payload");
    CheckEq("malformed signature", odd.detail, "a malformed signature's detail");
  }

  // ---- the member entry and display ------------------------------------------
  CheckEq(R"({"client_id":"0x019b2e105a7c4d3e8f0123456789abcd","client_key":"0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"})",
          MemberEntry(kClientId, kClientKey), "the member entry");
  CheckEq(R"({"client_id":"0x019b2e105a7c4d3e8f0123456789abcd","client_key":"0x79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"})",
          MemberEntry("019B2E10-5A7C-4D3E-8F01-23456789ABCD", kClientKey), "the member entry's id is lower case");
  CheckEq("", MemberEntry(kClientId, ""), "no member entry without a key");
  CheckEq("", MemberEntry("", kClientKey), "no member entry without a client id");
  CheckEq("0x79b5562e\xE2\x80\xA6" "049664", Shorten(kClientKey), "a key shortened 10 / 6");
  CheckEq("0x1234", Shorten("0x1234"), "a short value as it is");
  CheckEq("top200_sign_error_invalid", std::string(ErrorKey(Verdict::Invalid)), "invalid's line");
  CheckEq("top200_sign_error_no_key", std::string(ErrorKey(Verdict::NoKey)), "no key's line");
  CheckEq("top200_sign_error_other_device", std::string(ErrorKey(Verdict::OtherDevice)), "other device's line");
  CheckEq("", std::string(ErrorKey(Verdict::Valid)), "a valid binding shows no error");
  CheckEq("", std::string(ErrorKey(Verdict::Empty)), "an empty box shows no error");
  Expect(IsSignature(kSignature), "the vector's signature is 0x + 128 hex");
  Expect(!IsSignature(kSignature.substr(0, kSignature.size() - 2)), "63 bytes is not a signature");

  // ---- the app link ----------------------------------------------------------
  {
    const Link link = ParseSignLink("urnetwork://top200/sign?binding=" + kBindingBase64Url);
    Expect(link.sign && link.ok, "the page's link decodes");
    CheckEq(kBinding, link.binding, "the link carries the binding, byte for byte");
    // and it only pre-fills: the decoded text goes through the same check
    CheckEq("Valid", VerdictName(Validate(link.binding, kClientId, kClientKey).verdict),
            "the link's binding validates like a paste");
  }
  {
    const Link link = ParseSignLink("urnetwork://top200/sign?from=page&binding=" + kBindingBase64Url + "&x=1");
    Expect(link.sign && link.ok && link.binding == kBinding, "other parameters are ignored");
  }
  {
    const Link link = ParseSignLink("urnetwork://top200/sign");
    Expect(link.sign && !link.ok, "no query: the screen opens with the invalid line");
  }
  {
    const Link link = ParseSignLink("urnetwork://top200/sign?other=1");
    Expect(link.sign && !link.ok, "a missing binding parameter");
  }
  {
    const Link link = ParseSignLink("urnetwork://top200/sign?binding=");
    Expect(link.sign && !link.ok, "an empty binding parameter");
  }
  {
    const Link link = ParseSignLink("urnetwork://top200/sign?binding=" + std::string(4100, 'A'));
    Expect(link.sign && !link.ok, "a binding parameter over 4096 characters");
    const Link limit = ParseSignLink("urnetwork://top200/sign?binding=" + std::string(4096, 'A'));
    Expect(limit.sign && limit.ok, "4096 characters decode (and fail later as not a binding)");
    CheckEq("Invalid", VerdictName(Validate(limit.binding, kClientId, kClientKey).verdict),
            "zero bytes are not a binding");
  }
  {
    const Link padded = ParseSignLink("urnetwork://top200/sign?binding=w6k=");
    Expect(padded.sign && !padded.ok, "padding is not base64url without padding");
    const Link plus = ParseSignLink("urnetwork://top200/sign?binding=a+b/");
    Expect(plus.sign && !plus.ok, "'+' and '/' are base64, not base64url");
    const Link escaped = ParseSignLink("urnetwork://top200/sign?binding=w6k%3D");
    Expect(escaped.sign && !escaped.ok, "a percent escape is not decoded");
    const Link length = ParseSignLink("urnetwork://top200/sign?binding=AAAAA");
    Expect(length.sign && !length.ok, "a length no base64url text has");
    const Link utf8 = ParseSignLink("urnetwork://top200/sign?binding=w6k");
    Expect(utf8.sign && utf8.ok && utf8.binding == "\xC3\xA9", "UTF-8 decodes");
    const Link bytes = ParseSignLink("urnetwork://top200/sign?binding=__4");
    Expect(bytes.sign && !bytes.ok, "bytes that are not UTF-8");
    const Link twice = ParseSignLink("urnetwork://top200/sign?binding=w6k&binding=" + kBindingBase64Url);
    Expect(twice.sign && !twice.ok, "a binding named twice");
  }
  Expect(!ParseSignLink("urnetwork://top200/signx?binding=w6k").sign, "another path is not the sign link");
  Expect(!ParseSignLink("urnetwork://onboarding/connect").sign, "another host is not the sign link");
  Expect(!ParseSignLink("ur://top200/sign?binding=w6k").sign, "the android scheme is not Windows'");
  Expect(ParseSignLink("urnetwork://top200/sign/?binding=w6k").ok, "a trailing slash");

  std::cout << (gCases - gFailures) << "/" << gCases << " top 200 sign checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
