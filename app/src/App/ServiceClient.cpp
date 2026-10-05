// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "ServiceClient.h"

#include "Log.h"

namespace urnw {

bool ServiceClient::Connect() {
  pipe_.SetEventHandler([this](const nlohmann::json& event) {
    if (event.value("event", "") == "tunnel_state" && onState_) {
      if (auto it = event.find("status"); it != event.end()) {
        onState_(it->get<proto::TunnelStatus>());
      }
    }
  });
  pipe_.SetDisconnectHandler([this] {
    if (onDisconnect_) onDisconnect_();
  });
  return pipe_.Connect();
}

proto::TunnelStatus ServiceClient::CallStatus(const nlohmann::json& request,
                                              bool* answered) {
  proto::TunnelStatus status;
  if (answered) *answered = false;
  try {
    nlohmann::json reply = pipe_.Call(request);
    proto::Reply r = reply.get<proto::Reply>();
    if (r.status) status = *r.status;
    // "The service described its own state to us." A reply that carried no
    // status at all leaves the default-constructed struct in place, and that
    // struct reads as "nothing installed" — a claim about this machine that
    // nobody made. Only a status the service actually sent may be believed.
    if (answered) *answered = r.ok && r.status.has_value();
    if (!r.ok && !r.error.empty()) {
      status.state = proto::TunnelState::Error;
      status.error = r.error;
    }
  } catch (const std::exception& e) {
    LogError("service: call failed: {}", e.what());
    status.state = proto::TunnelState::Error;
    status.error = e.what();
  }
  return status;
}

proto::TunnelStatus ServiceClient::Hello() {
  return CallStatus(proto::Request(proto::msg::kHello));
}

proto::TunnelStatus ServiceClient::StartTunnel(const proto::StartTunnel& config) {
  nlohmann::json body = config;
  return CallStatus(proto::Request(proto::msg::kStartTunnel, body));
}

proto::TunnelStatus ServiceClient::StopTunnel() {
  return CallStatus(proto::Request(proto::msg::kStopTunnel));
}

proto::TunnelStatus ServiceClient::GetState(bool* answered) {
  return CallStatus(proto::Request(proto::msg::kGetState), answered);
}

bool ServiceClient::SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist) {
  proto::SetSplitTunnel s;
  s.excluded_app_paths = excludedPaths;
  s.allowlist_mode = allowlist;
  nlohmann::json body = s;
  try {
    nlohmann::json reply = pipe_.Call(proto::Request(proto::msg::kSetSplitTunnel, body));
    return reply.value("ok", false);
  } catch (const std::exception& e) {
    LogError("service: set split tunnel failed: {}", e.what());
    return false;
  }
}

bool ServiceClient::SetKillSwitch(bool on) {
  proto::SetKillSwitch s;
  s.on = on;
  nlohmann::json body = s;
  try {
    nlohmann::json reply = pipe_.Call(proto::Request(proto::msg::kSetKillSwitch, body));
    return reply.value("ok", false);
  } catch (const std::exception& e) {
    LogError("service: set kill switch failed: {}", e.what());
    return false;
  }
}

bool ServiceClient::Logout() {
  try {
    nlohmann::json reply = pipe_.Call(proto::Request(proto::msg::kLogout));
    return reply.value("ok", false);
  } catch (const std::exception& e) {
    LogError("service: logout failed: {}", e.what());
    return false;
  }
}

bool ServiceClient::StartProvider(const proto::StartProvider& request,
                                  std::optional<proto::TunnelStatus>* status,
                                  std::string* error) {
  nlohmann::json body = request;
  return CallProvider(proto::Request(proto::msg::kStartProvider, body), status, error);
}

bool ServiceClient::StopProvider(std::optional<proto::TunnelStatus>* status,
                                 std::string* error) {
  return CallProvider(proto::Request(proto::msg::kStopProvider), status, error);
}

bool ServiceClient::GetProviderStats(proto::ProviderStats& stats) {
  try {
    proto::Reply r = pipe_.Call(proto::Request(proto::msg::kGetProviderStats)).get<proto::Reply>();
    // An older service's "unknown request type" is a reply too: not ok, and no
    // statistics in it.
    if (!r.ok || !r.provider_stats) return false;
    stats = std::move(*r.provider_stats);
    return true;
  } catch (const std::exception& e) {
    LogError("service: get provider stats failed: {}", e.what());
    return false;
  }
}

bool ServiceClient::SetProvideExtender(bool on, std::string* error) {
  proto::SetProvideExtender s;
  s.provide_extender = on;
  nlohmann::json body = s;
  return CallProvider(proto::Request(proto::msg::kSetProvideExtender, body), nullptr, error);
}

bool ServiceClient::CallProvider(const nlohmann::json& request,
                                 std::optional<proto::TunnelStatus>* status,
                                 std::string* error) {
  if (status) status->reset();
  try {
    proto::Reply r = pipe_.Call(request).get<proto::Reply>();
    if (status) *status = r.status;
    if (error) *error = r.error;
    return r.ok;
  } catch (const std::exception& e) {
    LogError("service: {} failed: {}", proto::TypeOf(request), e.what());
    if (error) *error = e.what();
    return false;
  }
}

}  // namespace urnw
