// Executable spec for the network country on the control pipe
// (Common/Protocol.h, open bug P052): the set_network_country verb, the
// network_country_* fields of start_tunnel and start_provider and an older
// app's silence, the comparison that keeps a running provider-only device when
// only the country moves, and what the service makes of a payload
// (Common/NetworkCountry.h Normalized). And start_tunnel's system_proxy, the
// other fact about this PC's network the app hands the service for the log
// feedback uploads (Common/DiagnosticLines.h). Run against the same header the
// service and the app compile; it needs nlohmann/json, like the app.
//
//   c++ -std=c++20 -I ../src/Common -I <dir with nlohmann/json.hpp> network-country-protocol-tests.cpp -o /tmp/network-country-protocol-tests && /tmp/network-country-protocol-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "DiagnosticLines.h"
#include "NetworkCountry.h"
#include "Protocol.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

// Counts a case, and prints `what` when it fails.
void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;
  }
}

// set_network_country on the wire, and what the service takes off it.
void TestVerb() {
  Check(std::string(proto::msg::kSetNetworkCountry) == "set_network_country", "pipe: the verb's tag");

  proto::SetNetworkCountry country;
  country.network_country_code = "ru";
  country.network_country_source = "mobile-broadband";
  const nlohmann::json request = proto::Request(proto::msg::kSetNetworkCountry, country);
  Check(proto::TypeOf(request) == "set_network_country", "pipe: set_network_country is enveloped");
  const auto back = request.get<proto::SetNetworkCountry>();
  Check(back.network_country_code == "ru" && back.network_country_source == "mobile-broadband",
        "pipe: set_network_country round-trips");

  const auto silent = nlohmann::json::object().get<proto::SetNetworkCountry>();
  Check(silent.network_country_code.empty() && silent.network_country_source.empty(),
        "pipe: an empty set_network_country is no country");

  // what TunnelController::SetNetworkCountry takes off it
  const netcountry::Reading taken =
      netcountry::Normalized(back.network_country_code, back.network_country_source);
  Check(taken.code == "ru" && taken.source == netcountry::kSourceMobileBroadband,
        "pipe: the service takes the country and its source");
  nlohmann::json garbage = {{"type", "set_network_country"},
                            {"network_country_code", "Russia"},
                            {"network_country_source", "the locale"}};
  const auto peer = garbage.get<proto::SetNetworkCountry>();
  const netcountry::Reading refused =
      netcountry::Normalized(peer.network_country_code, peer.network_country_source);
  Check(refused.code.empty() && refused.source == netcountry::kSourceUnknown,
        "pipe: a peer's garbage is no country and no source");
}

// start_tunnel's network country, and an older app's silence.
void TestStartTunnel() {
  proto::StartTunnel start;
  start.instance_id = "instance";
  start.network_country_code = "ru";
  start.network_country_source = "mobile-broadband";
  nlohmann::json startJson = start;
  const auto startBack = startJson.get<proto::StartTunnel>();
  Check(startBack.network_country_code == "ru" && startBack.network_country_source == "mobile-broadband",
        "pipe: start_tunnel carries the network country");
  startJson.erase("network_country_code");
  startJson.erase("network_country_source");
  const auto older = startJson.get<proto::StartTunnel>();
  Check(older.network_country_code.empty() && older.network_country_source.empty() &&
            older.instance_id == "instance",
        "pipe: an older app's start_tunnel reads as no country");
}

// start_tunnel's system proxy kind, an older app's silence, and a proxy list
// where a kind belongs.
void TestStartTunnelSystemProxy() {
  proto::StartTunnel start;
  start.instance_id = "instance";
  start.system_proxy = "pac+manual-loopback";
  nlohmann::json startJson = start;
  const auto back = startJson.get<proto::StartTunnel>();
  Check(back.system_proxy == "pac+manual-loopback", "pipe: start_tunnel carries the system proxy kind");
  Check(diag::ProxyLine(back.system_proxy) == "user=pac+manual-loopback",
        "pipe: the service writes the kind it was sent");
  startJson.erase("system_proxy");
  const auto older = startJson.get<proto::StartTunnel>();
  Check(older.system_proxy.empty() && diag::ProxyLine(older.system_proxy) == "user=unknown",
        "pipe: an older app's start_tunnel reads as an unknown proxy");
  startJson["system_proxy"] = "http=203.0.113.9:8080";
  Check(diag::ProxyLine(startJson.get<proto::StartTunnel>().system_proxy) == "user=unknown",
        "pipe: a proxy list where a kind belongs is never written");
}

// start_provider's network country, which never rebuilds a running device.
void TestStartProvider() {
  proto::StartProvider provider;
  provider.by_jwt = "jwt";
  provider.network_space_json = "{}";
  provider.instance_id = "instance";
  provider.provide_mode = "always";
  provider.network_country_code = "ru";
  provider.network_country_source = "mobile-broadband";
  nlohmann::json providerJson = provider;
  const auto providerBack = providerJson.get<proto::StartProvider>();
  Check(providerBack.network_country_code == "ru" &&
            providerBack.network_country_source == "mobile-broadband",
        "pipe: start_provider carries the network country");
  providerJson.erase("network_country_code");
  providerJson.erase("network_country_source");
  Check(providerJson.get<proto::StartProvider>().network_country_code.empty(),
        "pipe: an older app's start_provider reads as no country");

  proto::StartProvider moved = provider;
  moved.network_country_code.clear();
  moved.network_country_source = "not-mobile-broadband";
  Check(proto::SameProviderDevice(provider, moved),
        "pipe: a new country keeps the running provider-only device (applied in place)");
  moved.instance_id = "another";
  Check(!proto::SameProviderDevice(provider, moved), "pipe: another identity still rebuilds it");
}

}  // namespace

int main() {
  TestVerb();
  TestStartTunnel();
  TestStartTunnelSystemProxy();
  TestStartProvider();
  std::cout << (gCases - gFailures) << "/" << gCases << " network country protocol checks passed"
            << std::endl;
  return gFailures == 0 ? 0 : 1;
}
