// What the page says about itself, and whether the app believes it.
//
// The action "Continue in browser" is offered only if the page that approves a
// sign-in is live and speaks this app's contract. The page publishes a small
// descriptor for that (contract v1, section 1): a GET of the build constant
// config::kApprovalDescriptorUrl answers, as application/json,
//   {"contracts":[1,2],"platforms":["windows"]}
// and the app offers the action only if, in this run, it fetched and parsed that
// within three seconds and `contracts` is an array that holds the number of the
// version the network needs (1 for the official network, 2 for another one:
// ApprovalNetwork.h) and `platforms` is an array that holds the string "windows".
// A page that lists only 1 keeps serving the official network and is never sent a
// version 2 request. Keys it does not know are ignored (a later version may add
// some); anything else, a body that is not JSON, a content type that is not
// application/json, a list without the entry, an entry of the wrong type, a body
// that is too large, means no.
//
// This is the pure half, the judgment of a response; ApprovalCheck.cpp makes the
// request (one GET, no redirect, three seconds) and caches the answer for the run.
//
// Needs nlohmann/json, like the other readers of a server's JSON (Common/ReleaseJson.h),
// and nothing of Windows: tools/approval-descriptor-tests.cpp runs it on any host
// that has the library.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ApprovalNetwork.h"

namespace urnw::approval {

// The row of the page's platform table this app is. (The contract versions it speaks, kContract and kContractNamed, are
// ApprovalNetwork.h's: they are also what decides which request is built.)
inline constexpr std::string_view kPlatform = "windows";
// The whole check - connect, request, answer, read - gets this long, once per run.
inline constexpr std::chrono::milliseconds kCheckBudget{3000};
// The descriptor is a few dozen bytes; anything over this is not it.
inline constexpr std::size_t kMaxDescriptorBytes = 4096;

enum class DescriptorVerdict {
  Offered,           // the page is live and lists this contract and this platform
  TooLarge,          // the body is over kMaxDescriptorBytes
  WrongContentType,  // not application/json
  NotJson,           // not a JSON document, or not an object
  NoContracts,       // `contracts` is missing, not an array, or has no number 1
  NoPlatform,        // `platforms` is missing, not an array, or has no "windows"
};

// The tokens for the log: closed and hyphenated or plain words that are not string ids.
constexpr std::string_view ToString(DescriptorVerdict verdict) {
  switch (verdict) {
    case DescriptorVerdict::Offered: return "offered";
    case DescriptorVerdict::TooLarge: return "too-large";
    case DescriptorVerdict::WrongContentType: return "wrong-content-type";
    case DescriptorVerdict::NotJson: return "not-json";
    case DescriptorVerdict::NoContracts: return "no-contract";
    case DescriptorVerdict::NoPlatform: return "no-platform";
  }
  return "unknown-descriptor";
}

namespace detail {

constexpr char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// A header value's media type is application/json, whatever its case, optionally
// followed by parameters ("; charset=utf-8"). application/jsonp, text/json and
// application/json-seq are other types.
constexpr bool IsJsonContentType(std::string_view contentType) {
  while (!contentType.empty() && (contentType.front() == ' ' || contentType.front() == '\t')) contentType.remove_prefix(1);
  constexpr std::string_view kJson = "application/json";
  if (contentType.size() < kJson.size()) return false;
  for (std::size_t i = 0; i < kJson.size(); ++i) {
    if (Lower(contentType[i]) != kJson[i]) return false;
  }
  std::string_view rest = contentType.substr(kJson.size());
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
  return rest.empty() || rest.front() == ';';
}

}  // namespace detail

// The verdict on one response. `contentType` is the Content-Type header as received.
inline DescriptorVerdict CheckDescriptor(std::string_view contentType, std::string_view body,
                                         std::int64_t contract = kContract, std::string_view platform = kPlatform) {
  if (body.size() > kMaxDescriptorBytes) return DescriptorVerdict::TooLarge;
  if (!detail::IsJsonContentType(contentType)) return DescriptorVerdict::WrongContentType;
  const nlohmann::json document = nlohmann::json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions=*/false);
  if (document.is_discarded() || !document.is_object()) return DescriptorVerdict::NotJson;

  const auto contracts = document.find("contracts");
  bool listsContract = false;
  if (contracts != document.end() && contracts->is_array()) {
    for (const nlohmann::json& entry : *contracts) {
      if (entry.is_number_integer() && entry.get<std::int64_t>() == contract) listsContract = true;
    }
  }
  if (!listsContract) return DescriptorVerdict::NoContracts;

  const auto platforms = document.find("platforms");
  bool listsPlatform = false;
  if (platforms != document.end() && platforms->is_array()) {
    for (const nlohmann::json& entry : *platforms) {
      if (entry.is_string() && entry.get<std::string>() == platform) listsPlatform = true;
    }
  }
  return listsPlatform ? DescriptorVerdict::Offered : DescriptorVerdict::NoPlatform;
}

// Which of the versions this app speaks a response lists. The action is offered in a network only if the page lists the
// version that network needs, so the page is asked once and the answer is read per version (Lists).
struct Listing {
  DescriptorVerdict verdict = DescriptorVerdict::NotJson;  // Offered if the page lists this platform and either version
  std::uint32_t contracts = 0;                            // bit n is set when the page lists version n

  constexpr bool Lists(std::int64_t contract) const {
    return contract >= 0 && contract < 32 && ((contracts >> contract) & 1U) != 0;
  }
};

// One judgment per version, by CheckDescriptor, so that this and the single-version judgment cannot disagree about what
// a descriptor is. When neither version is offered the word is the official network's, except that a platform the page
// lacks is the more telling one when the other version's list was in order.
inline Listing ReadDescriptor(std::string_view contentType, std::string_view body, std::string_view platform = kPlatform) {
  const DescriptorVerdict official = CheckDescriptor(contentType, body, kContract, platform);
  const DescriptorVerdict named = CheckDescriptor(contentType, body, kContractNamed, platform);
  Listing out;
  if (official == DescriptorVerdict::Offered) out.contracts |= 1U << kContract;
  if (named == DescriptorVerdict::Offered) out.contracts |= 1U << kContractNamed;
  if (out.contracts != 0) {
    out.verdict = DescriptorVerdict::Offered;
  } else {
    out.verdict = named == DescriptorVerdict::NoPlatform ? DescriptorVerdict::NoPlatform : official;
  }
  return out;
}

}  // namespace urnw::approval
