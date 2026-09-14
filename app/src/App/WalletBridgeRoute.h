// Which waiting flow a wallet-bridge return belongs to. The bridge is a pair of
// process-wide callbacks with no request id, so a return nobody waits for (the
// bridge page's "Return to URnetwork" after its automatic redirect, a tab from
// an abandoned or superseded attempt) is dropped, never turned into a sign-in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

namespace urnw::bridge {

// every reason SdkHost::CancelPendingWalletFlows answers with starts with this
inline constexpr std::string_view kSupersededPrefix = "superseded by ";

constexpr bool IsSuperseded(std::string_view error) noexcept {
  return error.starts_with(kSupersededPrefix);
}

enum class PublicKeyRoute { Drop, AnswerConnect, SignForRequest, SignIn };

constexpr PublicKeyRoute RoutePublicKey(bool bittensor, bool connectWaiting, bool signWaiting,
                                        bool walletSignInWaiting) noexcept {
  if (bittensor) return PublicKeyRoute::Drop;  // no connect hop to chain
  if (connectWaiting) return PublicKeyRoute::AnswerConnect;
  if (signWaiting) return PublicKeyRoute::SignForRequest;
  return walletSignInWaiting ? PublicKeyRoute::SignIn : PublicKeyRoute::Drop;
}

enum class SignatureRoute { Drop, AnswerRequest, SignIn };

constexpr SignatureRoute RouteSignature(bool signWaiting, bool walletSignInWaiting) noexcept {
  if (signWaiting) return SignatureRoute::AnswerRequest;
  return walletSignInWaiting ? SignatureRoute::SignIn : SignatureRoute::Drop;
}

}  // namespace urnw::bridge
