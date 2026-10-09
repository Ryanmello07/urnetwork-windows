// The GitHub releases list, read into ReleaseSelection.h's plain structs.
//
// One reader for the update checker and the elevated update helper, so the two
// can never disagree about what a release says. The list is another service's
// JSON, and its shape is exactly what this must survive rather than assume:
// nlohmann's value() substitutes a default only for a missing key, and a key
// present with the wrong type ("tag_name": null) throws out of get<>(). So
// every read is a find and a type check, a malformed release degrades to
// fields left empty (which SelectRelease then skips), and a body that is not
// an array is no list at all. Nothing here throws.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ReleaseSelection.h"

namespace urnw::update {

namespace json_detail {

inline bool Flag(nlohmann::json const& object, const char* field) {
  const auto it = object.find(field);
  return it != object.end() && it->is_boolean() && it->get<bool>();
}

inline std::string Text(nlohmann::json const& object, const char* field) {
  const auto it = object.find(field);
  if (it == object.end() || !it->is_string()) return {};
  return it->get<std::string>();
}

}  // namespace json_detail

// The releases of a list body, in its order; nullopt when the body is not a
// JSON array.
inline std::optional<std::vector<Release>> ParseReleaseList(std::string_view body) {
  // parse(..., false): a malformed body comes back as discarded, not a throw
  const nlohmann::json list = nlohmann::json::parse(body.begin(), body.end(), nullptr, false);
  if (!list.is_array()) return std::nullopt;
  std::vector<Release> releases;
  for (auto const& item : list) {
    if (!item.is_object()) continue;
    Release release;
    release.tag = json_detail::Text(item, "tag_name");
    release.draft = json_detail::Flag(item, "draft");
    release.prerelease = json_detail::Flag(item, "prerelease");
    release.immutable = json_detail::Flag(item, "immutable");
    release.publishedAt = json_detail::Text(item, "published_at");
    if (auto assets = item.find("assets"); assets != item.end() && assets->is_array()) {
      for (auto const& asset : *assets) {
        if (!asset.is_object()) continue;
        release.assets.push_back({.name = json_detail::Text(asset, "name"),
                                  .url = json_detail::Text(asset, "browser_download_url"),
                                  .digest = json_detail::Text(asset, "digest")});
      }
    }
    releases.push_back(std::move(release));
  }
  return releases;
}

}  // namespace urnw::update
