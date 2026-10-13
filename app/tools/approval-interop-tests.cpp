// The page's own conformance vector, opened by the app's own code (contract v1,
// section 9): a sealed answer the page's library made from FIXED, NOT SECRET keys
// (approval-vector.h) must open with the sdk's GenerateSharedSecret and DecryptData,
// and a flipped byte or a wrong key must fail. Then the same for approval::Unseal
// (App/ApprovalUnseal.h), the one function the app opens a box with: it returns the
// code for the vector, and nothing, never an exception, for everything else - a wrong
// key, an altered box, another attempt's state, keys and boxes of the wrong length,
// plaintext that is not the page's JSON - and a round trip with key pairs the sdk
// generates, sealed here with the sdk's own EncryptData, opens too.
//
// This needs the sdk: urnetwork_sdk.hpp, nlohmann/json and the sdk library
// (URnetworkSdk.dll) it calls through the C ABI, so unlike the other specs it cannot run
// on any host. tests/ur_io_approval_interop_test.go builds and runs it where the sdk is
// (the repository vendors it under third_party/urnetwork-sdk, or URNETWORK_SDK_INCLUDE
// names another directory), and skips where it is not.
//
//   c++ -std=c++20 -I ../src/App -isystem <sdk dir> -isystem <nlohmann dir>
//       approval-interop-tests.cpp <sdk dir>/URnetworkSdk.dll -o approval-interop-tests
//   (one command; it is broken here only to keep the comment's lines short)
//   (run it with the sdk dir on PATH)
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <urnetwork_sdk.hpp>

#include "ApprovalUnseal.h"
#include "AuthLink.h"
#include "UrIoApproval.h"
#include "approval-vector.h"

using namespace urnw;

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

// bytes first, first + 1, ... : the vector's recipe for the keys
std::vector<uint8_t> Counting(int first, int count) {
  std::vector<uint8_t> out;
  for (int i = 0; i < count; ++i) out.push_back(static_cast<uint8_t>(first + i));
  return out;
}

std::string Base58(const std::vector<uint8_t>& bytes) {
  return urnet::encodeBase58(bytes.data(), static_cast<int32_t>(bytes.size()));
}

// Does the call throw? The sdk's wrapper throws urnet::Error for everything that goes wrong.
template <class F>
bool Throws(F&& call) {
  try {
    call();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

// A base58 character other than `c`, so a value changes without leaving the alphabet.
char OtherBase58(char c) { return c == '2' ? '3' : '2'; }

std::string Flipped(std::string value, std::size_t at) {
  value[at] = OtherBase58(value[at]);
  return value;
}

const std::string kExpectedCode = std::string(683, 'A') + "=";
const std::string kState(approval_vector::kState);

authlink::Return VectorLink() {
  authlink::Return link;
  link.kind = authlink::Kind::Sealed;
  link.reject = authlink::Reject::None;
  link.state = kState;
  link.epk = std::string(approval_vector::kEpk);
  link.nonce = std::string(approval_vector::kNonce);
  link.data = std::string(approval_vector::kData);
  return link;
}

// The shared secret of the vector's attempt, base58, as the app computes it.
std::string VectorShared() {
  const std::optional<std::vector<uint8_t>> epk = urnet::decodeBase58(std::string(approval_vector::kEpk));
  if (!epk) return {};
  const std::vector<uint8_t> shared = urnet::generateSharedSecret(Counting(1, 32), *epk);
  return Base58(shared);
}

void TestTheSdkOpensThePagesVector() {
  const std::string shared = VectorShared();
  Check(!shared.empty(), "vector: the shared secret of the app's key (bytes 1 to 32) and the page's epk");
  std::string opened;
  try {
    const std::vector<uint8_t> plain = urnet::decryptData(std::string(approval_vector::kData), std::string(approval_vector::kNonce), shared);
    opened.assign(plain.begin(), plain.end());
  } catch (const std::exception& e) {
    Check(false, std::string("vector: the sdk opens the page's box (it threw: ") + e.what() + ")");
  }
  const nlohmann::json json = nlohmann::json::parse(opened, nullptr, /*allow_exceptions=*/false);
  Check(json.is_object() && json.value("c", "") == kExpectedCode && json.value("s", "") == kState,
        "vector: the plaintext is {\"c\": 683 'A' and '=', \"s\": 43 'S'}");

  // a flipped byte of the box, of the nonce, or of the page's public key: the sdk throws
  const std::string data(approval_vector::kData);
  const std::string nonce(approval_vector::kNonce);
  for (const std::size_t at : {std::size_t{0}, data.size() / 2, data.size() - 1}) {
    Check(Throws([&] { urnet::decryptData(Flipped(data, at), nonce, shared); }),
          "vector: a changed character at place " + std::to_string(at) + " of the box fails to open");
  }
  Check(Throws([&] { urnet::decryptData(data, Flipped(nonce, 3), shared); }), "vector: a changed nonce fails to open");
  Check(Throws([&] { urnet::decryptData(data.substr(0, data.size() - 5), nonce, shared); }), "vector: a truncated box fails to open");
  // a wrong key: the bytes 2 to 33 instead of 1 to 32
  const std::optional<std::vector<uint8_t>> epk = urnet::decodeBase58(std::string(approval_vector::kEpk));
  const std::string wrong = Base58(urnet::generateSharedSecret(Counting(2, 32), *epk));
  Check(Throws([&] { urnet::decryptData(data, nonce, wrong); }), "vector: a wrong key fails to open");
  // and the sdk throws for a key of the wrong length: the reason every call is guarded
  Check(Throws([&] { urnet::generateSharedSecret(Counting(1, 31), *epk); }), "vector: a secret key of 31 bytes makes the sdk throw");
  Check(Throws([&] { urnet::generateSharedSecret(Counting(1, 32), Counting(1, 31)); }), "vector: a public key of 31 bytes makes the sdk throw");
  Check(Throws([&] { urnet::decryptData(data, "1", shared); }), "vector: a nonce of the wrong length makes the sdk throw");
}

// The request vector (approval-vector.h, kPublicKey) is the public half of the app's secret key, the bytes 1 to 32: the page's
// one-time secret key (the bytes 101 to 132) and that key make the very shared secret that the app's key and the page's epk make.
// Two keys that are not a pair do not. This is what makes the request that the app builds for the vector the request of the
// same attempt whose answer the vector opens.
void TestTheRequestVectorsKeyIsTheAppsPublicKey() {
  const std::optional<std::vector<uint8_t>> pk = urnet::decodeBase58(std::string(approval_vector::kPublicKey));
  Check(pk && pk->size() == 32, "request vector: the public key is 32 bytes of base58");
  if (!pk) return;
  const std::string viaPage = Base58(urnet::generateSharedSecret(Counting(101, 32), *pk));
  Check(!viaPage.empty() && viaPage == VectorShared(),
        "request vector: the page's secret key and the vector's pk make the shared secret that the app's key and the page's epk make");
  std::vector<uint8_t> other = *pk;
  other[0] ^= 1;
  Check(Base58(urnet::generateSharedSecret(Counting(101, 32), other)) != VectorShared(),
        "request vector: and a key that is one bit off does not");
  Check(Base58(urnet::generateSharedSecret(Counting(102, 32), *pk)) != VectorShared(),
        "request vector: nor does the pk with another secret key of the page");
}

void TestTheAppsUnsealOpensItAndNothingElse() {
  const std::string secretKey = Base58(Counting(1, 32));
  const authlink::Return link = VectorLink();

  std::optional<std::string> code;
  try {
    code = approval::Unseal(secretKey, link);
  } catch (...) {
    Check(false, "unseal: an exception left the function for the page's own vector");
  }
  Check(code && *code == kExpectedCode, "unseal: the page's vector gives the code, and only the code");

  // each of these fails, and none throws
  const auto fails = [&](const std::string& key, const authlink::Return& changed, const std::string& what) {
    bool escaped = false;
    std::optional<std::string> result;
    try {
      result = approval::Unseal(key, changed);
    } catch (...) {
      escaped = true;
    }
    Check(!escaped, "unseal: " + what + " lets an exception out");
    Check(!escaped && !result, "unseal: " + what + " is no answer");
  };
  fails(Base58(Counting(2, 32)), link, "a wrong secret key");
  fails(Base58(Counting(1, 31)), link, "a secret key of 31 bytes");
  fails(Base58(Counting(1, 33)), link, "a secret key of 33 bytes");
  fails("", link, "no secret key");
  fails("0OIl", link, "a secret key that is not base58");
  fails(std::string(5000, '1'), link, "a secret key of 5000 characters");
  for (const std::size_t at : {std::size_t{0}, link.data.size() / 2, link.data.size() - 1}) {
    authlink::Return changed = link;
    changed.data = Flipped(changed.data, at);
    fails(secretKey, changed, "a box changed at place " + std::to_string(at));
  }
  {
    authlink::Return changed = link;
    changed.nonce = Flipped(changed.nonce, 5);
    fails(secretKey, changed, "a changed nonce");
    changed = link;
    changed.nonce = "1";
    fails(secretKey, changed, "a nonce of one byte");
    changed = link;
    changed.nonce = "";
    fails(secretKey, changed, "no nonce");
    changed = link;
    changed.epk = Flipped(changed.epk, 7);
    fails(secretKey, changed, "a changed public key of the page");
    changed = link;
    changed.epk = "1";
    fails(secretKey, changed, "a public key of one byte");
    changed = link;
    changed.epk = "";
    fails(secretKey, changed, "no public key of the page");
    changed = link;
    changed.data = "";
    fails(secretKey, changed, "no box");
    changed = link;
    changed.data = link.data.substr(0, 40);
    fails(secretKey, changed, "a box cut short");
    changed = link;
    changed.data = std::string(2048, 'z');
    fails(secretKey, changed, "a box of nothing");
    changed = link;
    changed.state = std::string(43, 'T');
    fails(secretKey, changed, "an answer to some other attempt's state (the box holds the state it was sealed for)");
    changed = link;
    changed.state = "";
    fails(secretKey, changed, "an empty state");
  }
}

// Sealed here with the sdk's own EncryptData under the vector's shared secret, so the plaintexts that are
// not the page's JSON can be tried: the box opens, and what is in it is the question.
void TestWhatIsInsideTheBoxDecides() {
  const std::string secretKey = Base58(Counting(1, 32));
  const std::string shared = VectorShared();
  const std::string nonce = Base58(Counting(177, 24));
  const auto sealed = [&](const std::string& plaintext) {
    authlink::Return link = VectorLink();
    link.nonce = nonce;
    link.data = urnet::encryptData(reinterpret_cast<const uint8_t*>(plaintext.data()), static_cast<int32_t>(plaintext.size()), nonce, shared);
    return link;
  };
  const auto opens = [&](const std::string& plaintext) {
    try {
      return approval::Unseal(secretKey, sealed(plaintext));
    } catch (...) {
      Check(false, "unseal: an exception left the function for " + plaintext.substr(0, 30));
      return std::optional<std::string>();
    }
  };
  const std::string good = "{\"c\":\"A-CODE\",\"s\":\"" + kState + "\"}";
  Check(opens(good) == std::optional<std::string>("A-CODE"), "inside: the page's JSON with another code gives that code");
  Check(opens("{\"s\":\"" + kState + "\",\"c\":\"A-CODE\"}") == std::optional<std::string>("A-CODE"), "inside: either order of keys");
  Check(opens("{\"c\":\"A-CODE\",\"s\":\"" + kState + "\",\"x\":1}") == std::optional<std::string>("A-CODE"),
        "inside: a field it does not know is ignored");
  Check(!opens("{\"c\":\"A-CODE\",\"s\":\"" + std::string(43, 'T') + "\"}"), "inside: another state is no answer");
  Check(!opens("{\"c\":\"A-CODE\",\"s\":\"" + kState.substr(1) + "\"}"), "inside: a state one short is no answer");
  Check(!opens("{\"c\":\"A-CODE\",\"s\":\"" + kState + "S\"}"), "inside: a state one long is no answer");
  Check(!opens("{\"c\":\"A-CODE\"}"), "inside: no state");
  Check(!opens("{\"s\":\"" + kState + "\"}"), "inside: no code");
  Check(!opens("{\"c\":\"\",\"s\":\"" + kState + "\"}"), "inside: an empty code");
  Check(!opens("{\"c\":5,\"s\":\"" + kState + "\"}"), "inside: a code that is not a string");
  Check(!opens("{\"c\":\"A-CODE\",\"s\":5}"), "inside: a state that is not a string");
  Check(!opens("{\"c\":null,\"s\":null}"), "inside: nulls");
  Check(!opens("[\"A-CODE\",\"" + kState + "\"]"), "inside: an array");
  Check(!opens("\"A-CODE\""), "inside: a string");
  Check(!opens("not json"), "inside: not JSON");
  Check(!opens("{\"c\":\"A-CODE\",\"s\":\"" + kState + "\""), "inside: JSON cut short");
  Check(!opens("{}"), "inside: an empty object");
  Check(!opens(std::string("{\"c\":\"") + std::string(2049, 'A') + "\",\"s\":\"" + kState + "\"}"), "inside: a code over the cap");
  Check(opens(std::string("{\"c\":\"") + std::string(2048, 'A') + "\",\"s\":\"" + kState + "\"}") ==
            std::optional<std::string>(std::string(2048, 'A')),
        "inside: a code at the cap");
  Check(opens("{\"c\":\"" + kExpectedCode + "\",\"s\":\"" + kState + "\"}") == std::optional<std::string>(kExpectedCode),
        "inside: a code as long as the server's (683 characters and the padding)");
}

// The whole round trip with key pairs the sdk generates: the app's pair, the page's one-time pair, the box made
// with the sdk's own EncryptData under the page's side of the shared secret, opened by the app's side.
void TestARoundTripWithGeneratedKeys() {
  int opened = 0;
  int pairs = 0;
  int widths[3] = {0, 0, 0};
  for (int round = 0; round < 40; ++round) {
    // the app's pair is the one the app itself makes (approval::NewKeyPair, which says which half is public): a pair
    // with its halves swapped puts the secret key in the request, and nothing the page seals could then be opened
    const std::optional<approval::KeyPair> app = approval::FreshKeyPair(approval::NewKeyPair);
    const std::optional<urnet::WalletKeyPair> page = urnet::generateWalletKeyPair();
    if (!app || !page) continue;
    ++pairs;
    ++widths[app->publicKey.size() == 43 ? 0 : (app->publicKey.size() == 44 ? 1 : 2)];
    const auto appSecret = urnet::decodeBase58(app->secretKey);
    const auto appPublic = urnet::decodeBase58(app->publicKey);
    const auto pageSecret = urnet::decodeBase58(page->PrivateKeyBase58);
    if (!appSecret || !appPublic || !pageSecret || appSecret->size() != 32 || appPublic->size() != 32) continue;
    // the page's side: its one-time secret and the app's public key
    const std::string pageShared = Base58(urnet::generateSharedSecret(*pageSecret, *appPublic));
    const std::string nonce = Base58(Counting(round + 1, 24));
    const std::string plaintext = "{\"c\":\"CODE" + std::to_string(round) + "\",\"s\":\"" + kState + "\"}";
    authlink::Return link;
    link.kind = authlink::Kind::Sealed;
    link.reject = authlink::Reject::None;
    link.state = kState;
    link.epk = page->PublicKeyBase58;
    link.nonce = nonce;
    link.data = urnet::encryptData(reinterpret_cast<const uint8_t*>(plaintext.data()), static_cast<int32_t>(plaintext.size()), nonce, pageShared);
    const std::optional<std::string> code = approval::Unseal(app->secretKey, link);
    if (code && *code == "CODE" + std::to_string(round)) ++opened;
    // another attempt's key does not open it
    const std::optional<approval::KeyPair> other = approval::FreshKeyPair(approval::NewKeyPair);
    if (other) Check(!approval::Unseal(other->secretKey, link), "round trip: another attempt's key does not open the box");
    // and the request's key is not the one that opens it: the public half is no secret key
    Check(!approval::Unseal(app->publicKey, link), "round trip: the public key, which the request carries, does not open the box");
  }
  Check(pairs >= 30, "round trip: the sdk generated key pairs the page accepts (" + std::to_string(pairs) + " of 40)");
  Check(opened == pairs, "round trip: every box the page side sealed opened on the app's side (" + std::to_string(opened) + " of " + std::to_string(pairs) + ")");
  Check(widths[2] == 0, "round trip: every public key the model took was 43 or 44 characters");
}

// The pair the attempt holds: the public half is the one that goes to the browser, and the secret half opens the
// answer. Fabricated pairs, so that a swap or a short key shows at once, whatever the sdk would have made.
void TestTheKeyPairHasItsHalvesInTheirPlaces() {
  urnet::WalletKeyPair made;
  made.PrivateKeyBase58 = Base58(Counting(1, 32));
  made.PublicKeyBase58 = Base58(Counting(101, 32));
  const std::optional<approval::KeyPair> pair = approval::KeyPairFrom(made);
  Check(pair && pair->publicKey == made.PublicKeyBase58, "key pair: the request carries the sdk's public half");
  Check(pair && pair->secretKey == made.PrivateKeyBase58, "key pair: and the attempt keeps the private half to open the answer");
  Check(pair && pair->publicKey != made.PrivateKeyBase58 && pair->secretKey != made.PublicKeyBase58,
        "key pair: neither half is the other");
  const auto refused = [&](const std::string& privateKey, const std::string& publicKey, const std::string& what) {
    urnet::WalletKeyPair odd;
    odd.PrivateKeyBase58 = privateKey;
    odd.PublicKeyBase58 = publicKey;
    bool escaped = false;
    std::optional<approval::KeyPair> result;
    try {
      result = approval::KeyPairFrom(odd);
    } catch (...) {
      escaped = true;
    }
    Check(!escaped && !result, "key pair: " + what + " is no pair");
  };
  refused(Base58(Counting(1, 31)), made.PublicKeyBase58, "a private key of 31 bytes");
  refused(Base58(Counting(1, 33)), made.PublicKeyBase58, "a private key of 33 bytes");
  refused(made.PrivateKeyBase58, Base58(Counting(101, 31)), "a public key of 31 bytes");
  refused(made.PrivateKeyBase58, Base58(Counting(101, 33)), "a public key of 33 bytes");
  refused("", made.PublicKeyBase58, "no private key");
  refused(made.PrivateKeyBase58, "", "no public key");
  refused("0OIl", made.PublicKeyBase58, "a private key that is not base58");
  refused(made.PrivateKeyBase58, "0OIl", "a public key that is not base58");
  refused(std::string(5000, '1'), made.PublicKeyBase58, "a private key of 5000 zero bytes");
  // what the sdk makes is a pair
  const std::optional<approval::KeyPair> real = approval::NewKeyPair();
  Check(real && !real->publicKey.empty() && !real->secretKey.empty() && real->publicKey != real->secretKey,
        "key pair: the sdk's own pair, through the function the app calls");
  // the halves belong together: what is sealed to the public half is opened by the secret one, by an
  // exchange with a second pair (each side's shared secret is the same, and only for the right halves)
  const std::optional<approval::KeyPair> other = approval::NewKeyPair();
  if (real && other) {
    const auto realSecret = urnet::decodeBase58(real->secretKey);
    const auto realPublic = urnet::decodeBase58(real->publicKey);
    const auto otherSecret = urnet::decodeBase58(other->secretKey);
    const auto otherPublic = urnet::decodeBase58(other->publicKey);
    if (realSecret && realPublic && otherSecret && otherPublic) {
      Check(urnet::generateSharedSecret(*realSecret, *otherPublic) == urnet::generateSharedSecret(*otherSecret, *realPublic),
            "key pair: each pair's secret half and the other's public half make the same shared secret");
      Check(urnet::generateSharedSecret(*realPublic, *otherPublic) != urnet::generateSharedSecret(*otherSecret, *realPublic),
            "key pair: and a public half taken for a secret one does not");
    }
  }
}

// A public key of small order, as the page's `epk`, makes the shared secret ONE constant whatever the app's
// secret key, and the sdk does not refuse it (libsodium does). Sealed under that constant by anyone who knows
// only the attempt's state, a box would open with nothing of the page's behind it. The premise is checked
// first, with the sdk's own calls: the box does open. Then the app's Unseal must say no.
void TestAPublicKeyOfSmallOrderIsNoKeyAtAll() {
  const std::string secretKey = Base58(Counting(1, 32));
  const std::vector<uint8_t> secretBytes = Counting(1, 32);
  const std::string nonce = Base58(Counting(177, 24));
  const std::string plaintext = "{\"c\":\"FORGED-CODE\",\"s\":\"" + kState + "\"}";
  struct Point {
    std::string name;
    std::vector<uint8_t> bytes;
  };
  const auto edge = [](uint8_t first, uint8_t middle, uint8_t last) {
    std::vector<uint8_t> bytes(32, middle);
    bytes[0] = first;
    bytes[31] = last;
    return bytes;
  };
  const std::vector<Point> points = {
      {"0", edge(0x00, 0x00, 0x00)},
      {"1", edge(0x01, 0x00, 0x00)},
      {"p - 1", edge(0xec, 0xff, 0x7f)},
      {"p", edge(0xed, 0xff, 0x7f)},
      {"p + 1", edge(0xee, 0xff, 0x7f)},
      {"the first point of order 8",
       {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
        0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00}},
      {"the second point of order 8",
       {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1, 0x55, 0x9c, 0x83, 0xef, 0x5b,
        0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c, 0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57}},
  };
  for (const Point& point : points) {
    // the attacker's side: any secret key of his own gives the same constant
    const std::string constant = Base58(urnet::generateSharedSecret(Counting(51, 32), point.bytes));
    authlink::Return link = VectorLink();
    link.epk = Base58(point.bytes);
    link.nonce = nonce;
    link.data = urnet::encryptData(reinterpret_cast<const uint8_t*>(plaintext.data()), static_cast<int32_t>(plaintext.size()), nonce, constant);
    // the premise: the sdk's own calls open it with the app's secret key, so nothing but the app's check stands in the way
    std::string opened;
    try {
      const std::vector<uint8_t> plain =
          urnet::decryptData(link.data, nonce, Base58(urnet::generateSharedSecret(secretBytes, point.bytes)));
      opened.assign(plain.begin(), plain.end());
    } catch (const std::exception&) {
    }
    Check(opened == plaintext, "small order: the sdk itself opens a box sealed under the constant of the point " + point.name);
    std::optional<std::string> code;
    bool escaped = false;
    try {
      code = approval::Unseal(secretKey, link);
    } catch (...) {
      escaped = true;
    }
    Check(!escaped && !code, "small order: the app's Unseal refuses the point " + point.name);
  }
  // the page's own key still opens: the refusal is of those points and no others
  Check(approval::Unseal(secretKey, VectorLink()) == std::optional<std::string>(kExpectedCode),
        "small order: and the page's real key, as ever, opens the vector");
}

}  // namespace

int main() {
  try {
    TestTheSdkOpensThePagesVector();
    TestTheRequestVectorsKeyIsTheAppsPublicKey();
    TestTheAppsUnsealOpensItAndNothingElse();
    TestWhatIsInsideTheBoxDecides();
    TestTheKeyPairHasItsHalvesInTheirPlaces();
    TestAPublicKeyOfSmallOrderIsNoKeyAtAll();
    TestARoundTripWithGeneratedKeys();
  } catch (const std::exception& e) {
    std::cout << "  FAIL an exception left a test: " << e.what() << "\n";
    return 1;
  }
  std::cout << (gCases - gFailures) << "/" << gCases << " approval interop checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
