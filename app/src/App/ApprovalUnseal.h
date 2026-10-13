// Opening the page's sealed answer: the one place the app opens a box that came off
// a link, and the one place it makes the key pair the box is sealed to.
//
// The page seals the auth code it minted to the attempt's public key (contract v1,
// section 3): a NaCl box (curve25519-xsalsa20-poly1305) of the JSON
//   {"c":"<auth code>","s":"<state>"}
// from a one-time key pair of its own, whose public half is the link's `epk`, with
// the link's nonce `n`; `epk`, `n` and the box `d` are base58. The app computes the
// shared secret from the attempt's secret key and `epk` and opens the box with the
// sdk's own GenerateSharedSecret and DecryptData, then compares the box's own `s`
// with the link's state byte for byte.
//
// Anything but that is no answer: a key, a nonce or a box of the wrong length, a box
// that does not open under this key (sealed for another attempt, or altered), a
// plaintext that is not that JSON, an inner state that differs, and an `epk` that is a
// point of small order. The sdk's crypto THROWS (urnet::Error) for the first three, and
// the box came off a link that any program can fire at the app, so every exception is
// caught here and is no answer; nothing leaves this function but a code or nothing.
//
// A public key of small order (the point 0 or 1, p - 1, p, p + 1, and the two of order
// eight, in any of their encodings) makes the shared secret ONE constant, whatever the
// secret key it is multiplied by, and the sdk's GenerateSharedSecret does not refuse
// those as libsodium does. A box sealed under that constant opens with nothing of the
// page's behind it, by anyone who knows only the attempt's state (which rides on the
// browser's command line), so it is refused here: the constant is what the zero point
// gives with the same secret key, and a shared secret that equals it is no proof of a
// key at all.
//
// What the function holds on the way - the secret key as bytes and as text, the shared
// secret as bytes and as text, the plaintext - is overwritten before it returns, on
// every path, a throw included (the sdk's own copies, and the JSON values the parser
// made from the plaintext, cannot be reached, which SdkHost.h says of seedphrases too).
//
// It needs the sdk (urnetwork_sdk.hpp and, to run, the sdk library), so unlike the
// other headers of the approval it is not run by a pure host test: it is run against
// the page's own conformance vector by tools/approval-interop-tests.cpp, wherever the
// sdk is, and tests/auth_link_wiring_test.go pins its text.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <urnetwork_sdk.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AuthLink.h"
#include "UrIoApproval.h"

namespace urnw::approval {

namespace detail {

// Runs `f` when it goes out of scope, a return and a throw alike.
template <class F>
class OnExit {
 public:
  explicit OnExit(F f) : f_(std::move(f)) {}
  OnExit(const OnExit&) = delete;
  OnExit& operator=(const OnExit&) = delete;
  ~OnExit() { f_(); }

 private:
  F f_;
};

// Two byte strings are the same, compared without an early exit: both are secrets.
inline bool SameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return SameSecret(std::string_view(reinterpret_cast<const char*>(a.data()), a.size()),
                    std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}

}  // namespace detail

// The attempt's key pair from the sdk's, base58. The PUBLIC half is what the request carries to
// the browser; the SECRET half opens the page's answer and never leaves the app. This is the one
// place that says which is which, and tools/approval-interop-tests.cpp runs it: against fabricated
// pairs (a swap shows at once) and against the sdk, where the page's side of a round trip is sealed
// to the half this puts in the request, so a pair with its halves swapped does not open. Nothing
// when a half does not decode to the 32 bytes of a key: a request that carries a public key the
// page cannot parse would only look like a page that does nothing.
inline std::optional<KeyPair> KeyPairFrom(const urnet::WalletKeyPair& made) {
  try {
    std::optional<std::vector<uint8_t>> secretBytes = urnet::decodeBase58(made.PrivateKeyBase58);
    const std::optional<std::vector<uint8_t>> publicBytes = urnet::decodeBase58(made.PublicKeyBase58);
    const bool whole = secretBytes && secretBytes->size() == 32 && publicBytes && publicBytes->size() == 32;
    if (secretBytes) Wipe(*secretBytes);
    if (!whole) return std::nullopt;
    return KeyPair{made.PublicKeyBase58, made.PrivateKeyBase58};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// A new key pair from the sdk (KeyPairFrom), nothing when the sdk fails. The sdk's own copy of the
// secret key is overwritten as soon as this has its own (its copies inside the library are out of
// reach, as SdkHost.h says of seedphrases).
inline std::optional<KeyPair> NewKeyPair() {
  try {
    std::optional<urnet::WalletKeyPair> made = urnet::generateWalletKeyPair();
    const detail::OnExit overwrite{[&] {
      if (made) Wipe(made->PrivateKeyBase58);
    }};
    if (!made) return std::nullopt;
    return KeyPairFrom(*made);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

inline std::optional<std::string> Unseal(std::string_view ownSecret, const authlink::Return& link) {
  std::string secretText(ownSecret);
  std::optional<std::vector<uint8_t>> secret;
  std::vector<uint8_t> shared, constant, plain;
  std::string sharedBase58;
  const detail::OnExit overwrite{[&] {
    Wipe(secretText);
    if (secret) Wipe(*secret);
    Wipe(shared);
    Wipe(constant);
    Wipe(plain);
    Wipe(sharedBase58);
  }};
  try {
    secret = urnet::decodeBase58(secretText);
    const std::optional<std::vector<uint8_t>> sealedBy = urnet::decodeBase58(link.epk);
    if (!secret || !sealedBy) return std::nullopt;
    shared = urnet::generateSharedSecret(*secret, *sealedBy);
    constant = urnet::generateSharedSecret(*secret, std::vector<uint8_t>(32, 0));
    if (shared.empty() || detail::SameBytes(shared, constant)) return std::nullopt;
    sharedBase58 = urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
    plain = urnet::decryptData(link.data, link.nonce, sharedBase58);
    const nlohmann::json opened = nlohmann::json::parse(plain.begin(), plain.end(), nullptr, false);
    if (!opened.is_object()) return std::nullopt;
    const auto code = opened.find("c");
    const auto state = opened.find("s");
    if (code == opened.end() || state == opened.end() || !code->is_string() || !state->is_string()) return std::nullopt;
    if (!SameSecret(state->get<std::string>(), link.state)) return std::nullopt;
    std::string answer = code->get<std::string>();
    if (answer.empty() || answer.size() > 2048) return std::nullopt;
    return answer;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace urnw::approval
