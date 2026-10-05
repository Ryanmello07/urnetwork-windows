// Executable spec for the provider-only device on the control pipe
// (Common/Protocol.h): the start_provider and stop_provider tags, the
// StartProvider request on the wire, the TunnelStatus provider_* fields and
// their reading from a service too old to send them, the request comparison
// that keeps a running device (SameProviderDevice), and what one status tells
// the app's reconcile (ProviderFactsFrom). Run against the SAME header the
// service and the app compile; it needs nlohmann/json, like the app.
//
//   c++ -std=c++20 -I ../src/Common -I <dir with nlohmann/json.hpp> provide-protocol-tests.cpp -o /tmp/provide-protocol-tests && /tmp/provide-protocol-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "Protocol.h"

using namespace urnw;
using namespace urnw::proto;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

StartProvider SampleRequest() {
  StartProvider r;
  r.by_jwt = "client.jwt.value";
  r.network_space_json = R"({"key":{"host_name":"bringyour.com","env_name":"main"}})";
  r.instance_id = "0193e520-acfa-9c76-fb85-6e1f862d1d9b";
  r.device_description = "DESKTOP-1";
  r.device_spec = "windows amd64";
  r.app_version = "2026.10.5-1";
  r.provide_mode = "always";
  r.provider_transport_settings_json = R"({"mode":"auto"})";
  return r;
}

// (a) the verbs
void TestTags() {
  Check(std::string(msg::kStartProvider) == "start_provider", "tags: start_provider");
  Check(std::string(msg::kStopProvider) == "stop_provider", "tags: stop_provider");
  const nlohmann::json request = Request(msg::kStartProvider, SampleRequest());
  Check(TypeOf(request) == "start_provider", "tags: the request carries its type");
}

// (b) the request survives the wire, and an absent field is empty, not a throw
void TestStartProviderJson() {
  const StartProvider sent = SampleRequest();
  const nlohmann::json wire = nlohmann::json::parse(DumpForWire(nlohmann::json(sent)));
  const StartProvider got = wire.get<StartProvider>();
  Check(got.by_jwt == sent.by_jwt, "request: by_jwt");
  Check(got.network_space_json == sent.network_space_json, "request: network_space_json");
  Check(got.instance_id == sent.instance_id, "request: instance_id");
  Check(got.device_description == sent.device_description, "request: device_description");
  Check(got.device_spec == sent.device_spec, "request: device_spec");
  Check(got.app_version == sent.app_version, "request: app_version");
  Check(got.provide_mode == sent.provide_mode, "request: provide_mode");
  Check(got.provider_transport_settings_json == sent.provider_transport_settings_json,
        "request: provider_transport_settings_json");
  // no RPC material: the provider-only device has no listener to pin
  for (const char* rpc : {"rpc_server_pem", "rpc_client_cert_pem", "rpc_listen_hostport",
                          "rpc_session_id"}) {
    Check(!wire.contains(rpc), std::string("request: carries no ") + rpc);
  }
  const StartProvider sparse =
      nlohmann::json::parse(R"({"provide_mode":"auto","by_jwt":null})").get<StartProvider>();
  Check(sparse.provide_mode == "auto" && sparse.by_jwt.empty() &&
            sparse.provider_transport_settings_json.empty(),
        "request: absent and null fields read as empty");
}

// (c) the status fields, and a service too old to send them
void TestStatusFields() {
  TunnelStatus status;
  status.provider_running = true;
  status.provider_control_mode = "auto";
  status.provider_mode = 1;
  status.provider_network_key = true;
  const nlohmann::json wire = nlohmann::json::parse(DumpForWire(nlohmann::json(status)));
  for (const char* key : {"provider_running", "provider_control_mode", "provider_mode",
                          "provider_network_key"}) {
    Check(wire.contains(key), std::string("status: writes ") + key);
  }
  const TunnelStatus got = wire.get<TunnelStatus>();
  Check(got.provider_running, "status: provider_running round-trips");
  Check(got.provider_control_mode == "auto", "status: provider_control_mode round-trips");
  Check(got.provider_mode == 1, "status: provider_mode round-trips");
  Check(got.provider_network_key, "status: provider_network_key round-trips");

  // an older service: no provider fields at all reads as "no provider-only
  // device", which claims less than the truth
  const TunnelStatus old = nlohmann::json::parse(
                               R"({"state":"stopped","wfp_state":"off","protocol_version":4})")
                               .get<TunnelStatus>();
  Check(!old.provider_running && old.provider_control_mode.empty() && old.provider_mode == 0 &&
            !old.provider_network_key,
        "status: an older service reads as no provider-only device");
  Check(kProtocolVersion == 4, "status: no protocol bump for the provider verbs and fields");

  // a refused start_provider still carries the status beside its reason
  Reply refused;
  refused.ok = false;
  refused.error = provide::RefusalReason(provide::ProviderRefusal::KillSwitchArmed);
  refused.in_reply_to = msg::kStartProvider;
  TunnelStatus armed;
  armed.wfp_state = "armed";
  refused.status = armed;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(refused))).get<Reply>();
  Check(!back.ok && back.error == refused.error && back.status &&
            back.status->wfp_state == "armed" && !back.status->provider_running,
        "status: a refusal carries its reason and the status");
}

// (d) the comparison that keeps a running device
void TestSameProviderDevice() {
  const StartProvider base = SampleRequest();
  StartProvider remoded = base;
  remoded.provide_mode = "network";
  Check(SameProviderDevice(base, remoded), "same device: only the mode differs");
  Check(SameProviderDevice(base, base), "same device: identical");

  struct Change {
    const char* field;
    void (*apply)(StartProvider&);
  };
  const Change changes[] = {
      {"by_jwt", [](StartProvider& r) { r.by_jwt = "refreshed.jwt"; }},
      {"network_space_json", [](StartProvider& r) { r.network_space_json = "{}"; }},
      {"instance_id",
       [](StartProvider& r) { r.instance_id = "0193e520-acfa-9c76-fb85-000000000001"; }},
      {"device_description", [](StartProvider& r) { r.device_description = "DESKTOP-2"; }},
      {"device_spec", [](StartProvider& r) { r.device_spec = "windows arm64"; }},
      {"app_version", [](StartProvider& r) { r.app_version = "2026.10.6-1"; }},
      {"provider_transport_settings_json",
       [](StartProvider& r) { r.provider_transport_settings_json = ""; }},
  };
  for (const Change& change : changes) {
    StartProvider other = base;
    change.apply(other);
    Check(!SameProviderDevice(base, other),
          std::string("same device: a different ") + change.field + " builds a new device");
  }
}

// (e) what one status tells the app's reconcile
void TestProviderFacts() {
  TunnelStatus idle;
  idle.state = TunnelState::Stopped;
  idle.wfp_state = "off";
  const provide::ServiceProviderFacts answered = ProviderFactsFrom(idle, true);
  Check(answered.answered && !answered.tunnelSession && !answered.killSwitchArmed &&
            !answered.providerRunning,
        "facts: an idle service");
  Check(!ProviderFactsFrom(idle, false).answered, "facts: answered is the transport's word");

  for (TunnelState state : {TunnelState::Starting, TunnelState::Preparing, TunnelState::Up,
                            TunnelState::RpcOnly, TunnelState::Stopping}) {
    TunnelStatus session = idle;
    session.state = state;
    Check(ProviderFactsFrom(session, true).tunnelSession,
          std::string("facts: ") + ToString(state) + " is a tunnel session");
  }
  for (TunnelState state : {TunnelState::Stopped, TunnelState::Error}) {
    TunnelStatus none = idle;
    none.state = state;
    Check(!ProviderFactsFrom(none, true).tunnelSession,
          std::string("facts: ") + ToString(state) + " is no tunnel session");
  }

  for (const char* wfp : {"armed", "connecting", "connected"}) {
    TunnelStatus policy = idle;
    policy.wfp_state = wfp;
    Check(ProviderFactsFrom(policy, true).killSwitchArmed,
          std::string("facts: wfp ") + wfp + " is a policy in force");
  }
  for (const char* wfp : {"off", ""}) {
    TunnelStatus open = idle;
    open.wfp_state = wfp;
    Check(!ProviderFactsFrom(open, true).killSwitchArmed,
          std::string("facts: wfp '") + wfp + "' is no policy");
  }

  TunnelStatus providing = idle;
  providing.provider_running = true;
  providing.provider_mode = 3;
  Check(ProviderFactsFrom(providing, true).providerRunning, "facts: the provider runs");
  // and the whole chain: an Always user who just disconnected starts providing
  Check(provide::DisconnectedProviderStep("always", ProviderFactsFrom(idle, true)) ==
            provide::DisconnectedStep::Start,
        "facts: Always after a Disconnect starts the provider-only device");
  TunnelStatus armedAfterDrop = idle;
  armedAfterDrop.wfp_state = "armed";
  Check(provide::DisconnectedProviderStep("always", ProviderFactsFrom(armedAfterDrop, true)) ==
            provide::DisconnectedStep::None,
        "facts: nothing starts under the armed floor");
}

}  // namespace

int main() {
  TestTags();
  TestStartProviderJson();
  TestStatusFields();
  TestSameProviderDevice();
  TestProviderFacts();
  std::cout << (gCases - gFailures) << "/" << gCases << " provide protocol checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
