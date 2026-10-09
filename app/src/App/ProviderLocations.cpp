// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose — see ProviderLocations.h. App.vcxproj compiles this with
// PrecompiledHeader=NotUsing.
#include "ProviderLocations.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace urnw {

std::string PlaceLabel(const ProviderLocationRow& row) {
  std::string label;
  for (const std::string* part : {&row.city, &row.region, &row.country}) {
    if (part->empty()) continue;
    if (!label.empty()) label += ", ";
    label += *part;
  }
  return label;
}

std::string CoordinatesLabel(const ProviderLocationRow& row) {
  if (!row.hasCoordinates) return "\xE2\x80\x94";  // em dash, utf-8
  char buffer[64];
  // C locale: the SDK coordinates are data, so the decimal separator stays '.'
  // in every language (android formats with Locale.US for the same reason)
  std::snprintf(buffer, sizeof(buffer), "%.4f, %.4f", row.lat, row.lon);
  return std::string(buffer);
}

namespace {

// ASCII case-insensitive equality: client ids are canonical lowercase uuids,
// but an id that went through another surface may not be
bool SameClientId(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

std::string Trimmed(const std::string& s) {
  const auto first = s.find_first_not_of(" \t");
  if (first == std::string::npos) return std::string();
  const auto last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}

}  // namespace

StayOnExitState StayOnExitStateFor(const ProviderLocationRow& row,
                                   const std::string& selectedClientId,
                                   const std::string& stayingClientId) {
  if (row.clientId.empty()) return StayOnExitState::None;
  if (SameClientId(row.clientId, stayingClientId)) return StayOnExitState::Staying;
  if (SameClientId(row.clientId, selectedClientId)) return StayOnExitState::Offer;
  return StayOnExitState::None;
}

std::string ShortClientId(const std::string& clientId) {
  const std::string id = Trimmed(clientId);
  if (id.size() <= 12) return id;
  return id.substr(0, 4) + "\xE2\x80\xA6" + id.substr(id.size() - 4);  // ellipsis, utf-8
}

std::string StayOnExitName(const ProviderLocationRow& row) {
  const std::string shortId = ShortClientId(row.clientId);
  std::string place;
  for (const std::string* part : {row.city.empty() ? &row.region : &row.city, &row.country}) {
    if (part->empty()) continue;
    if (!place.empty()) place += ", ";
    place += *part;
  }
  if (place.empty()) return shortId;
  return shortId + " \xC2\xB7 " + place;  // middle dot, utf-8
}

std::optional<StayOnExitTarget> MakeStayOnExitTarget(const ProviderLocationRow& row) {
  const std::string clientId = Trimmed(row.clientId);
  if (clientId.empty()) return std::nullopt;
  StayOnExitTarget target;
  target.clientId = clientId;
  target.name = StayOnExitName(row);
  target.city = row.city;
  target.region = row.region;
  target.country = row.country;
  target.countryCode = row.countryCode;
  return target;
}

ConnectedDuration SplitConnectedDuration(int64_t connectedSinceMillis, int64_t nowMillis) {
  if (connectedSinceMillis <= 0) return ConnectedDuration{};
  const int64_t elapsedSeconds = (std::max)(int64_t{0}, (nowMillis - connectedSinceMillis) / 1000);
  ConnectedDuration duration;
  duration.valid = true;
  duration.hours = elapsedSeconds / 3600;
  duration.minutes = (elapsedSeconds % 3600) / 60;
  duration.seconds = elapsedSeconds % 60;
  return duration;
}

}  // namespace urnw
