// last-result.json (UpdateResult.h), written by the update helper and read by
// the tray app. Nothing here throws: a file that is not exactly one report
// reads as none.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "UpdateResult.h"

namespace urnw::update {

inline std::string FormatUpdateResult(const UpdateResult& result) {
  const nlohmann::json object = {
      {"tag", result.tag},
      {"code", result.code},
      {"exitCode", result.exitCode},
      {"finishedUtc", result.finishedUtc},
  };
  return object.dump() + "\n";
}

// The report `text` holds: every field present with its type, and the values
// IsWellFormed accepts.
inline std::optional<UpdateResult> ParseUpdateResult(std::string_view text,
                                                     std::string_view tagPrefix = {}) {
  const nlohmann::json object = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
  if (!object.is_object()) return std::nullopt;
  const auto tag = object.find("tag");
  const auto code = object.find("code");
  const auto exitCode = object.find("exitCode");
  const auto finished = object.find("finishedUtc");
  if (tag == object.end() || !tag->is_string() || code == object.end() ||
      !code->is_number_unsigned() || exitCode == object.end() ||
      !exitCode->is_number_integer() || finished == object.end() || !finished->is_string()) {
    return std::nullopt;
  }
  UpdateResult result{.tag = tag->get<std::string>(),
                      .code = code->get<std::uint64_t>(),
                      .exitCode = exitCode->get<std::int64_t>(),
                      .finishedUtc = finished->get<std::string>()};
  if (!IsWellFormed(result, tagPrefix)) return std::nullopt;
  return result;
}

}  // namespace urnw::update
