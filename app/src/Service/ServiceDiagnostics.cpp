// SPDX-License-Identifier: MPL-2.0
#include "ServiceDiagnostics.h"

#include <cstdint>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Same order as EgressMonitor.h: winsock2 before the IP helpers.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "DiagnosticLines.h"
#include "NetworkConfig.h"  // NetworkConfig::DiscoverEgress, read when nothing is pinned
#include "Sdk.h"  // urnet::logAppInfo

namespace urnw {
namespace {

// A Windows constant as DiagnosticLines.h types it, for the checks below: the
// values that header restates so it can stay free of Windows headers.
constexpr uint32_t U32(auto value) { return static_cast<uint32_t>(value); }
static_assert(diag::kIfTypeEthernet == U32(IF_TYPE_ETHERNET_CSMACD));
static_assert(diag::kIfTypeWifi == U32(IF_TYPE_IEEE80211));
static_assert(diag::kPhysicalMediumWirelessLan == U32(NdisPhysicalMediumWirelessLan));
static_assert(diag::kPhysicalMediumNative80211 == U32(NdisPhysicalMediumNative802_11));

// The DNS client's Group Policy key (the DoHPolicy value lives here), and the
// Name Resolution Policy Table's two homes: Group Policy's, and the local one
// that Add-DnsClientNrptRule writes. Each rule is one subkey.
constexpr wchar_t kDnsClientPolicyKey[] = L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient";
constexpr wchar_t kNrptPolicyKey[] =
    L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient\\DnsPolicyConfig";
constexpr wchar_t kNrptLocalKey[] =
    L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DnsPolicyConfig";

// A DWORD under HKLM, none when the value is not set or is not a DWORD.
std::optional<uint32_t> ReadPolicyDword(const wchar_t* key, const wchar_t* value) {
  DWORD data = 0;
  DWORD size = sizeof(data);
  if (::RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) !=
      ERROR_SUCCESS)
    return std::nullopt;
  return static_cast<uint32_t>(data);
}

// The subkeys of one key: 0 when there is no key, -1 when it cannot be read.
int64_t CountSubkeys(const wchar_t* path) {
  HKEY key = nullptr;
  const LSTATUS open = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key);
  if (open == ERROR_FILE_NOT_FOUND) return 0;
  if (open != ERROR_SUCCESS) return -1;
  DWORD subkeys = 0;
  const LSTATUS info = ::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                          nullptr, nullptr, nullptr, nullptr, nullptr);
  ::RegCloseKey(key);
  return info == ERROR_SUCCESS ? static_cast<int64_t>(subkeys) : -1;
}

// The rules in both of the table's homes, -1 when either cannot be read.
int64_t CountNrptRules() {
  const int64_t policy = CountSubkeys(kNrptPolicyKey);
  const int64_t local = CountSubkeys(kNrptLocalKey);
  if (policy < 0 || local < 0) return -1;
  return policy + local;
}

// The kind of the interface the service's sdk is pinned to, from its type, its
// medium and its media state, and nothing else of it: no alias, description,
// address or GUID is read.
diag::InterfaceFacts InterfaceFactsFor(int64_t index) {
  diag::InterfaceFacts facts;
  if (index <= 0) return facts;
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = static_cast<NET_IFINDEX>(index);
  if (::GetIfEntry2(&row) != NO_ERROR) {
    facts.kind = diag::InterfaceKind::Other;
    return facts;
  }
  facts.kind = diag::InterfaceKindFor(row.Type, static_cast<uint32_t>(row.PhysicalMediumType));
  facts.connected = row.MediaConnectState != MediaConnectStateDisconnected;
  return facts;
}

}  // namespace

void ServiceDiagnostics::NoteStatus(const proto::TunnelStatus& status, bool killSwitch,
                                    const std::optional<netcountry::Reading>& networkCountry) {
  diag::ServiceFacts facts;
  facts.state = proto::ToString(status.state);
  facts.mode = proto::ToString(status.mode);
  facts.routes = status.routes_installed;
  facts.dns = status.dns_applied;
  facts.firewall = status.wfp_state;
  facts.killSwitch = killSwitch;
  facts.failsafeArmed = status.failsafe_armed;
  facts.stopReason = status.stop_reason;
  facts.providerRunning = status.provider_running;
  facts.providerControlMode = status.provider_control_mode;
  facts.providerTier = status.provider_mode;
  // The interfaces a session pins the sdk to; with none pinned (no session,
  // or a start that failed before it pinned one), the ones the default routes
  // take now, labelled observed.
  const bool pinned = status.egress_index4 > 0 || status.egress_index6 > 0;
  EgressInterfaces observed;
  if (!pinned) observed = NetworkConfig::DiscoverEgress(NET_LUID{});
  const std::string adapter = diag::AdapterLine(
      status.routes_installed, pinned ? diag::EgressSource::Pinned : diag::EgressSource::Observed,
      InterfaceFactsFor(pinned ? status.egress_index4 : observed.index4),
      InterfaceFactsFor(pinned ? status.egress_index6 : observed.index6));

  std::scoped_lock lock(mutex_);
  WriteIfChangedLocked(diag::kTagService, diag::ServiceLine(facts), lastService_);
  WriteIfChangedLocked(diag::kTagAdapter, adapter, lastAdapter_);
  // The DNS client settings and the network country matter to a session's
  // queries and dials, and are said when there is one: a session that comes
  // up says them, a later push of it says them again only if they changed,
  // and the end of it forgets them, so the next session says them too
  // (whatever has rotated out of the log meanwhile).
  if (proto::IsSessionLive(status.state)) {
    WriteIfChangedLocked(
        diag::kTagDns,
        diag::DnsLine(diag::DohPolicyFor(ReadPolicyDword(kDnsClientPolicyKey, L"DoHPolicy")),
                      CountNrptRules()),
        lastDns_);
    WriteNetworkCountryLocked(networkCountry);
  } else {
    lastDns_.clear();
    lastNetworkCountry_.clear();
  }
}

void ServiceDiagnostics::NoteStart(const proto::StartTunnel& request) {
  // Every start says it, unchanged or not: each is the user's own connect.
  std::scoped_lock lock(mutex_);
  urnet::logAppInfo(std::string(diag::kTagProxy), diag::ProxyLine(request.system_proxy));
}

void ServiceDiagnostics::NoteNetworkCountry(
    const std::optional<netcountry::Reading>& networkCountry) {
  std::scoped_lock lock(mutex_);
  WriteNetworkCountryLocked(networkCountry);
}

void ServiceDiagnostics::NoteLogUpload(std::string_view carrier) {
  // Every upload says it, unchanged or not: each is the user's own send, and
  // the line has to be in the zip that follows.
  std::scoped_lock lock(mutex_);
  urnet::logAppInfo(std::string(diag::kTagLogUpload), diag::LogUploadLine(carrier));
}

void ServiceDiagnostics::WriteNetworkCountryLocked(
    const std::optional<netcountry::Reading>& networkCountry) {
  // None before the app first sends one: nothing is known, so nothing is said.
  if (!networkCountry) return;
  WriteIfChangedLocked(diag::kTagNetworkCountry,
                       diag::NetworkCountryLine(networkCountry->code, networkCountry->source),
                       lastNetworkCountry_);
}

void ServiceDiagnostics::WriteIfChangedLocked(std::string_view tag, std::string line,
                                              std::string& last) {
  if (line == last) return;
  urnet::logAppInfo(std::string(tag), line);
  last = std::move(line);
}

}  // namespace urnw
