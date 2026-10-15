// Everything Earnings › Top 200 › Sign fleet binding decides before it touches a
// XAML object or the SDK: whether the text in the binding box is a Top 200 fleet
// binding of THIS device, the review it shows, the payload it copies, the member
// entry the fleet manifest needs, and what a urnetwork://top200/sign link
// pre-fills (support inbox 7934; factury support/plans/top200-in-app-signing.md).
//
// The flow: the fleet operator builds one binding per member device on the ur.io
// Top 200 page; the device's user pastes it here (or opens the page's app link,
// which only pre-fills the box), checks the review and taps Sign; the app signs
// it with the device's Ed25519 client key through the SDK's SignSnFleetBinding,
// which runs in the service over the existing device RPC, and shows
//   {"binding":<the pasted text, trimmed>,"client_signature":"0x<128 hex>"}
// built by concatenation, so a page-built binding gives a payload byte-identical
// to `provider fleet sign`. Nothing is sent anywhere by the app.
//
// Validation runs here first, before any SDK call, so the user sees why a
// binding is refused before anything is asked of the service: not a binding,
// another device's client_id or client_key, ambiguous JSON. The SDK repeats
// the device and JSON checks in the process that holds the key (sdk main
// 61e89201 refuses another device's client_key and duplicate, case-variant or
// extra keys); an older service checked only the client_id, which makes this
// the first line, and for such a service the only one, against a binding that
// would sign into a signature that never verifies.
//
// Header-only and WinRT-free, for the reason SnPayoutPresentation.h gives: a
// WinUI 3 app cannot be built off Windows, so tools/top200-sign-tests.cpp runs
// this on any host with a C++20 compiler and nlohmann/json. The sheet that uses
// it is Top200SignSheet.cpp.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace urnw::top200sign {

// The most characters a link's binding parameter may carry. A binding is about
// 600 characters of JSON, 800 as base64url.
inline constexpr size_t kMaxLinkBindingLength = 4096;

// The most characters of binding text that are parsed at all: a pasted
// clipboard can be anything, and a binding is about 600.
inline constexpr size_t kMaxBindingTextLength = 16 * 1024;

// The link the ur.io page's "Open in the app" fires on Windows.
inline constexpr std::string_view kSignLinkPrefix = "urnetwork://top200/sign";

// What the binding box holds, in the order the checks run.
enum class Verdict {
  // nothing (or only whitespace): no error, no review
  Empty,
  // not a binding: not JSON, a payload, a missing or malformed field
  Invalid,
  // the device has no client key to compare (the service is not connected)
  NoKey,
  // a binding, but its client_id or client_key is not this device's
  OtherDevice,
  // a binding of this device: show the review, enable Sign
  Valid,
};

// The binding's 11 fields (sn miner/fleet_sign.go fleetSignBinding), as the
// review shows them. Hex fields keep the text as pasted.
struct Binding {
  uint64_t chainId = 0;
  uint64_t netuid = 0;
  std::string coordinator;
  std::string fleetId;
  std::string hotkey;
  std::string clientId;
  std::string clientKey;
  uint64_t generation = 0;
  uint64_t validFromEpoch = 0;
  uint64_t validToEpoch = 0;
  std::string commitmentHash;
};

struct Check {
  Verdict verdict = Verdict::Empty;
  // the field text without surrounding whitespace: what is signed and wrapped
  std::string trimmed;
  // set from Valid, OtherDevice and NoKey on (the fields parsed)
  std::optional<Binding> binding;
};

// What a urnetwork://top200/sign link carries.
struct Link {
  // the link is the sign screen's (whatever its parameter)
  bool sign = false;
  // the binding parameter was present, at most kMaxLinkBindingLength
  // characters, base64url and UTF-8
  bool ok = false;
  // the decoded binding text, when ok
  std::string binding;
};

namespace detail {

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline bool IsHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

// "0x" and exactly `digits` hex digits (either case).
inline bool IsPrefixedHex(std::string_view s, size_t digits) {
  if (s.size() != 2 + digits || s[0] != '0' || s[1] != 'x') return false;
  for (size_t i = 2; i < s.size(); ++i) {
    if (!IsHexDigit(s[i])) return false;
  }
  return true;
}

// A client id as 32 lowercase hex digits: from a uuid (8-4-4-4-12) or from
// 0x + 32 hex. nullopt for anything else.
inline std::optional<std::string> ClientIdHex(std::string_view id) {
  if (IsPrefixedHex(id, 32)) return Lower(id.substr(2));
  if (id.size() != 36) return std::nullopt;
  std::string hex;
  for (size_t i = 0; i < id.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash) {
      if (id[i] != '-') return std::nullopt;
    } else {
      if (!IsHexDigit(id[i])) return std::nullopt;
      hex.push_back(id[i]);
    }
  }
  return Lower(hex);
}

// A JSON integer >= 0 (a float, a string, a negative number are not).
inline std::optional<uint64_t> Whole(nlohmann::json const& value) {
  if (value.is_number_unsigned()) return value.get<uint64_t>();
  if (value.is_number_integer()) {
    const int64_t v = value.get<int64_t>();
    if (v >= 0) return static_cast<uint64_t>(v);
  }
  return std::nullopt;
}

inline std::optional<std::string> Text(nlohmann::json const& value) {
  if (!value.is_string()) return std::nullopt;
  return value.get<std::string>();
}

// The value of one base64url character (RFC 4648 §5), or -1.
inline int Base64UrlValue(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

// Unpadded base64url to bytes; nullopt for any other character (padding
// included) or an impossible length.
inline std::optional<std::string> DecodeBase64Url(std::string_view s) {
  if (s.size() % 4 == 1) return std::nullopt;
  std::string out;
  out.reserve(s.size() * 3 / 4);
  uint32_t bits = 0;
  int count = 0;
  for (char c : s) {
    const int v = Base64UrlValue(c);
    if (v < 0) return std::nullopt;
    bits = (bits << 6) | static_cast<uint32_t>(v);
    count += 6;
    if (count >= 8) {
      count -= 8;
      out.push_back(static_cast<char>((bits >> count) & 0xFF));
    }
  }
  return out;
}

// Well-formed UTF-8: no stray continuation, overlong form, surrogate or code
// point past U+10FFFF.
inline bool IsUtf8(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t extra = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      ++i;
      continue;
    } else if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= s.size()) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

}  // namespace detail

// The text without leading and trailing ASCII whitespace.
inline std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && detail::IsSpace(text[begin])) ++begin;
  while (end > begin && detail::IsSpace(text[end - 1])) --end;
  return std::string(text.substr(begin, end - begin));
}

// Parses the binding box against this device's client id (uuid form, from
// Device.getClientId) and client key (Device.getSnClientKey, "" when the
// service is not connected). The checks run in the spec's order: a binding at
// all (exactly the 11 fields, spelled exactly, each once, nothing else, each
// well formed), then the key's presence, then the device's id and key. The
// spelling is strict because the SDK's Go decoder matches keys without case
// and lets a later one win: "hotkey" beside "HOTKEY" would show one hotkey in
// the review and sign the other.
inline Check Validate(std::string_view text, std::string_view deviceClientId,
                      std::string_view deviceClientKeyHex) {
  Check check;
  check.trimmed = Trim(text);
  if (check.trimmed.empty()) {
    check.verdict = Verdict::Empty;
    return check;
  }
  check.verdict = Verdict::Invalid;
  if (check.trimmed.size() > kMaxBindingTextLength) return check;
  // nothing before the object: nlohmann skips a UTF-8 byte order mark that Go
  // refuses, and the review must show only what the service will sign
  if (check.trimmed.front() != '{') return check;

  // a top-level field named twice is refused: the review must show the one
  // value every reader of the text agrees on
  bool duplicate = false;
  std::set<std::string> keys;
  const nlohmann::json::parser_callback_t onEvent =
      [&duplicate, &keys](int depth, nlohmann::json::parse_event_t event,
                          nlohmann::json& parsed) {
        if (event == nlohmann::json::parse_event_t::key && depth == 1 && parsed.is_string()) {
          if (!keys.insert(parsed.get<std::string>()).second) duplicate = true;
        }
        return true;
      };
  const nlohmann::json document =
      nlohmann::json::parse(check.trimmed, onEvent, /*allow_exceptions=*/false);
  if (document.is_discarded() || !document.is_object() || duplicate) return check;
  if (document.size() != 11) return check;

  auto named = [&document](char const* name) -> nlohmann::json const* {
    const auto it = document.find(name);
    return it == document.end() ? nullptr : &*it;
  };
  nlohmann::json const* chainId = named("chain_id");
  nlohmann::json const* netuid = named("netuid");
  nlohmann::json const* coordinator = named("coordinator");
  nlohmann::json const* fleetId = named("fleet_id");
  nlohmann::json const* hotkey = named("hotkey");
  nlohmann::json const* clientId = named("client_id");
  nlohmann::json const* clientKey = named("client_key");
  nlohmann::json const* generation = named("generation");
  nlohmann::json const* validFrom = named("valid_from_epoch");
  nlohmann::json const* validTo = named("valid_to_epoch");
  nlohmann::json const* commitment = named("commitment_hash");
  if (!chainId || !netuid || !coordinator || !fleetId || !hotkey || !clientId || !clientKey ||
      !generation || !validFrom || !validTo || !commitment) {
    return check;
  }

  Binding binding;
  {
    const auto chainValue = detail::Whole(*chainId);
    const auto netuidValue = detail::Whole(*netuid);
    const auto generationValue = detail::Whole(*generation);
    const auto fromValue = detail::Whole(*validFrom);
    const auto toValue = detail::Whole(*validTo);
    if (!chainValue || !netuidValue || !generationValue || !fromValue || !toValue) return check;
    if (*chainValue == 0 || *netuidValue > 65535 || *generationValue == 0 ||
        *toValue < *fromValue) {
      return check;
    }
    binding.chainId = *chainValue;
    binding.netuid = *netuidValue;
    binding.generation = *generationValue;
    binding.validFromEpoch = *fromValue;
    binding.validToEpoch = *toValue;
  }
  {
    const auto coordinatorText = detail::Text(*coordinator);
    const auto fleetText = detail::Text(*fleetId);
    const auto hotkeyText = detail::Text(*hotkey);
    const auto idText = detail::Text(*clientId);
    const auto keyText = detail::Text(*clientKey);
    const auto commitmentText = detail::Text(*commitment);
    if (!coordinatorText || !fleetText || !hotkeyText || !idText || !keyText || !commitmentText) {
      return check;
    }
    if (!detail::IsPrefixedHex(*coordinatorText, 40) || !detail::IsPrefixedHex(*fleetText, 64) ||
        !detail::IsPrefixedHex(*hotkeyText, 64) || !detail::IsPrefixedHex(*keyText, 64) ||
        !detail::IsPrefixedHex(*commitmentText, 64) || !detail::ClientIdHex(*idText)) {
      return check;
    }
    binding.coordinator = *coordinatorText;
    binding.fleetId = *fleetText;
    binding.hotkey = *hotkeyText;
    binding.clientId = *idText;
    binding.clientKey = *keyText;
    binding.commitmentHash = *commitmentText;
  }
  check.binding = binding;

  if (deviceClientKeyHex.empty()) {
    check.verdict = Verdict::NoKey;
    return check;
  }
  // the first line: the service's SDK compares them again (an older one only
  // the client id)
  const auto bindingId = detail::ClientIdHex(binding.clientId);
  const auto deviceId = detail::ClientIdHex(deviceClientId);
  if (!deviceId || bindingId != deviceId ||
      detail::Lower(binding.clientKey) != detail::Lower(deviceClientKeyHex)) {
    check.verdict = Verdict::OtherDevice;
    return check;
  }
  check.verdict = Verdict::Valid;
  return check;
}

// A signature SignSnFleetBinding can return: 0x + 128 hex (64 bytes).
inline bool IsSignature(std::string_view signature) {
  return detail::IsPrefixedHex(signature, 128);
}

// The payload the page takes under Device signature: the trimmed binding text
// wrapped as it was pasted, never re-serialized.
inline std::string Payload(std::string_view trimmedBinding, std::string_view signature) {
  std::string payload = "{\"binding\":";
  payload += trimmedBinding;
  payload += ",\"client_signature\":\"";
  payload += signature;
  payload += "\"}";
  return payload;
}

// What a signing ended with: the payload, or the detail top200_sign_error_failed
// shows (empty when nothing was signed because the binding was not Valid).
struct Signed {
  bool ok = false;
  std::string payload;
  std::string detail;
};

// Signs a checked binding with `signer` (the trimmed text in, the signature
// out; the sheet's calls Device.signSnFleetBinding, which throws on a refusal)
// and builds the payload. Only a Valid check reaches the signer. An empty
// signature is a failure with the detail "empty signature", one that is not
// 0x + 128 hex is "malformed signature", and a thrown error's text is the detail.
template <typename Signer>
Signed SignChecked(Check const& check, Signer&& signer) {
  Signed result;
  if (check.verdict != Verdict::Valid) return result;
  std::string signature;
  try {
    signature = signer(check.trimmed);
  } catch (std::exception const& e) {
    result.detail = e.what();
    if (result.detail.empty()) result.detail = "error";
    return result;
  } catch (...) {
    result.detail = "error";
    return result;
  }
  if (signature.empty()) {
    result.detail = "empty signature";
    return result;
  }
  if (!IsSignature(signature)) {
    result.detail = "malformed signature";
    return result;
  }
  result.ok = true;
  result.payload = Payload(check.trimmed, signature);
  return result;
}

// The device's fleet manifest member entry,
// {"client_id":"0x<32 lowercase hex>","client_key":"<key>"}, or "" when the id
// is not a uuid or the key is not 0x + 64 hex.
inline std::string MemberEntry(std::string_view deviceClientId, std::string_view deviceClientKeyHex) {
  const auto id = detail::ClientIdHex(deviceClientId);
  if (!id || !detail::IsPrefixedHex(deviceClientKeyHex, 64)) return std::string();
  return "{\"client_id\":\"0x" + *id + "\",\"client_key\":\"" + std::string(deviceClientKeyHex) +
         "\"}";
}

// A long hex value for display: the first 10 and the last 6 characters around
// an ellipsis ("0x79b5562e…049664"); short values as they are.
inline std::string Shorten(std::string_view value) {
  if (value.size() <= 17) return std::string(value);
  return std::string(value.substr(0, 10)) + "\xE2\x80\xA6" + std::string(value.substr(value.size() - 6));
}

// The store key of the line a verdict shows under the box, or "" for none.
inline std::string_view ErrorKey(Verdict verdict) {
  switch (verdict) {
    case Verdict::Invalid:
      return "top200_sign_error_invalid";
    case Verdict::NoKey:
      return "top200_sign_error_no_key";
    case Verdict::OtherDevice:
      return "top200_sign_error_other_device";
    case Verdict::Empty:
    case Verdict::Valid:
      break;
  }
  return std::string_view();
}

// Reads a urnetwork://top200/sign?binding=<base64url> link. `sign` says the
// link is the sign screen's; `ok` that its binding decoded. The parameter is
// read as it is, never percent-decoded: base64url needs no escape, so a '%' (or
// any character outside the alphabet) makes it not base64url. A binding named
// twice is refused rather than one of them picked.
inline Link ParseSignLink(std::string_view url) {
  Link link;
  if (url.substr(0, kSignLinkPrefix.size()) != kSignLinkPrefix) return link;
  std::string_view rest = url.substr(kSignLinkPrefix.size());
  if (!rest.empty() && rest[0] == '/') rest.remove_prefix(1);
  if (!rest.empty() && rest[0] != '?') return link;
  link.sign = true;
  if (rest.empty()) return link;
  rest.remove_prefix(1);

  std::optional<std::string_view> encoded;
  while (true) {
    const size_t amp = rest.find('&');
    const std::string_view pair = rest.substr(0, amp);
    const size_t eq = pair.find('=');
    if (eq != std::string_view::npos && pair.substr(0, eq) == "binding") {
      if (encoded) return link;
      encoded = pair.substr(eq + 1);
    }
    if (amp == std::string_view::npos) break;
    rest.remove_prefix(amp + 1);
  }
  if (!encoded || encoded->empty() || encoded->size() > kMaxLinkBindingLength) return link;
  auto decoded = detail::DecodeBase64Url(*encoded);
  if (!decoded || !detail::IsUtf8(*decoded)) return link;
  link.ok = true;
  link.binding = std::move(*decoded);
  return link;
}

}  // namespace urnw::top200sign
