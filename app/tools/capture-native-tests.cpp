// Elevated, destructive-to-network acceptance for a disposable Windows VM only.
// Uses production OS owners and the shared transaction, but injects provider
// proof. No SDK, account, provider, DNS lookup, or installed service is used.
// SPDX-License-Identifier: MPL-2.0
#include "../src/Service/NetworkConfig.h"
#include "../src/Service/Wintun.h"
#include "../src/Service/WfpPolicy.h"
#include "../src/Service/CaptureReadiness.h"
#include "Ids.h"
#include "../src/Service/NetPolicy.h"
#include "Strings.h"

#include <fwpmu.h>
#include <objbase.h>
#include <setupapi.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace urnw;

namespace {

constexpr wchar_t kAdapterName[] = L"UrnNativeCaptureTest";
constexpr GUID kAdapterGuid = {
    0x13f04798, 0x94af, 0x4baa, {0x9a, 0x40, 0x22, 0x36, 0x44, 0x55, 0x66, 0x77}};
constexpr DWORD kChildBudgetMillis = 60000;

// Failure messages contain only fixed operation names, never host snapshots.
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Handle {
  HANDLE value = nullptr;
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit Handle(HANDLE handle = nullptr) : value(handle) {}
};

std::wstring ExePath() {
  std::wstring path(32768, L'\0');
  const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  Check(size > 0 && size < path.size(), "cannot find test executable");
  path.resize(size);
  return path;
}

std::string Hex(const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (size_t i = 0; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4]);
    result.push_back(digits[bytes[i] & 15]);
  }
  return result;
}

std::string GuidKey(const GUID& guid) { return Hex(&guid, sizeof(guid)); }

std::string AddressKey(const SOCKADDR* address) {
  if (address->sa_family == AF_INET)
    return "4:" + Hex(&reinterpret_cast<const SOCKADDR_IN*>(address)->sin_addr, 4);
  if (address->sa_family == AF_INET6)
    return "6:" + Hex(&reinterpret_cast<const SOCKADDR_IN6*>(address)->sin6_addr, 16);
  throw std::runtime_error("unexpected address family in OS snapshot");
}

std::string LiteralKey(const char* literal, int family) {
  SOCKADDR_INET address{};
  address.si_family = static_cast<ADDRESS_FAMILY>(family);
  void* bytes = family == AF_INET ? static_cast<void*>(&address.Ipv4.sin_addr)
                                 : static_cast<void*>(&address.Ipv6.sin6_addr);
  Check(::InetPtonA(family, literal, bytes) == 1, "invalid synthetic address");
  return AddressKey(reinterpret_cast<const SOCKADDR*>(&address));
}

struct AdapterSnapshot {
  std::wstring alias;
  GUID guid{};
  std::set<std::string> dns;
  std::set<std::string> addresses;
  std::set<std::string> interfaces;
  bool operator==(const AdapterSnapshot&) const = default;
};

enum class DevicePresence { Absent, Present, Unknown };

struct NetworkSnapshot {
  std::map<uint64_t, std::set<std::string>> routes;
  std::map<uint64_t, AdapterSnapshot> adapters;
  // Retained for diagnosis, not live network state: exact test identity with
  // no IP state and independently proved absent from the present-device set.
  std::map<uint64_t, AdapterSnapshot> retiredBindings;
  std::map<uint64_t, DevicePresence> testDevicePresence;
  bool operator==(const NetworkSnapshot& other) const {
    return routes == other.routes && adapters == other.adapters;
  }
};

// Query present PnP devices, not persistent GUID/LUID/interface-name mappings.
// A failed/incomplete scan must never be interpreted as device absence.
DevicePresence ReadDevicePresence(const GUID& target) {
  constexpr GUID netClass = {
      0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
  const HDEVINFO devices = ::SetupDiGetClassDevsW(&netClass, nullptr, nullptr, DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE) return DevicePresence::Unknown;
  DevicePresence result = DevicePresence::Unknown;
  for (DWORD index = 0; ; ++index) {
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    if (!::SetupDiEnumDeviceInfo(devices, index, &device)) {
      if (::GetLastError() == ERROR_NO_MORE_ITEMS) result = DevicePresence::Absent;
      break;
    }
    if (index >= 512) break;
    const HKEY key = ::SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE) break;
    wchar_t text[64]{};
    DWORD type = 0, size = sizeof(text);
    const auto status = ::RegQueryValueExW(key, L"NetCfgInstanceId", nullptr, &type,
                                         reinterpret_cast<BYTE*>(text), &size);
    ::RegCloseKey(key);
    GUID guid{};
    if (status != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) || size > sizeof(text) ||
        size % sizeof(wchar_t) != 0 || text[size / sizeof(wchar_t) - 1] != L'\0' ||
        (std::wcslen(text) + 1) * sizeof(wchar_t) != size ||
        FAILED(::CLSIDFromString(text, &guid))) break;
    if (guid == target) {
      result = DevicePresence::Present;
      break;
    }
  }
  ::SetupDiDestroyDeviceInfoList(devices);
  return result;
}

// GAA_INCLUDE_ALL_INTERFACES can retain a removed adapter's alias-only record.
// Keep every live, unknown, foreign, or configured record in the strict gate.
template <typename Lookup>
void NormalizeRetiredTestBinding(NetworkSnapshot& snapshot, Lookup&& presence) {
  for (auto it = snapshot.adapters.begin(); it != snapshot.adapters.end();) {
    const auto& adapter = it->second;
    if (adapter.alias == kAdapterName && adapter.guid == kAdapterGuid &&
        adapter.dns.empty() && adapter.addresses.empty() && adapter.interfaces.empty() &&
        !snapshot.routes.contains(it->first)) {
      const auto state = presence(adapter.guid);
      snapshot.testDevicePresence[it->first] = state;
      if (state == DevicePresence::Absent) {
        snapshot.retiredBindings.emplace(*it);
        it = snapshot.adapters.erase(it);
        continue;
      }
    }
    ++it;
  }
}

// Same normalization seam as ReadNetwork, with only synthetic observations.
// The alias-only fixture fails the old unconditional baseline comparison.
void TestRetiredBindingDiscriminator() {
  const NetworkSnapshot baseline;
  AdapterSnapshot retired;
  retired.alias = kAdapterName;
  retired.guid = kAdapterGuid;
  NetworkSnapshot raw;
  raw.adapters.emplace(1, retired);
  Check(raw != baseline, "retired-binding fixture did not reproduce the old baseline mismatch");
  auto absent = [](const GUID&) { return DevicePresence::Absent; };
  auto normalized = raw;
  NormalizeRetiredTestBinding(normalized, absent);
  Check(normalized == baseline && normalized.retiredBindings.size() == 1,
        "proved retired test binding was not distinguished from a live adapter");
  for (const auto state : {DevicePresence::Present, DevicePresence::Unknown}) {
    auto snapshot = raw;
    NormalizeRetiredTestBinding(snapshot, [=](const GUID&) { return state; });
    Check(snapshot != baseline && snapshot.retiredBindings.empty(),
          "present or unknown test device was hidden by cleanup normalization");
    Check(snapshot.testDevicePresence.at(1) == state, "device-presence diagnostic was lost");
  }
  std::array<NetworkSnapshot, 7> live;
  live.fill(raw);
  live[0].routes[1].insert("synthetic-route");
  live[1].adapters.at(1).dns.insert("4:c0000235");
  live[2].adapters.at(1).addresses.insert("4:c0000201/24");
  live[3].adapters.at(1).interfaces.insert("2:1280:1:0");
  live[4].adapters.at(1).interfaces.insert("23:1280:1:0");
  live[5].adapters.at(1).alias = L"UrnOtherSyntheticAdapter";
  live[6].adapters.at(1).guid.Data1 ^= 1;
  for (auto snapshot : live) {
    NormalizeRetiredTestBinding(snapshot, absent);
    Check(snapshot != baseline && snapshot.retiredBindings.empty(),
          "configured or foreign adapter state was hidden by cleanup normalization");
  }
  auto healthy = baseline;
  NormalizeRetiredTestBinding(healthy, [](const GUID&) -> DevicePresence {
    throw std::runtime_error("healthy baseline unnecessarily queried device presence");
  });
  Check(healthy == baseline && healthy.retiredBindings.empty(), "healthy baseline changed");
  std::printf("PASS retired-binding discriminator: 11 controls; no OS changes\n");
}

// Ignore volatile route lifetimes/DAD progress, not destinations, next hops,
// interface ownership, resolver values, MTU, metric, or assigned addresses.
NetworkSnapshot ReadNetwork() {
  NetworkSnapshot result;
  MIB_IPFORWARD_TABLE2* routes = nullptr;
  Check(::GetIpForwardTable2(AF_UNSPEC, &routes) == NO_ERROR, "route enumeration failed");
  for (ULONG i = 0; i < routes->NumEntries; ++i) {
    const auto& row = routes->Table[i];
    result.routes[row.InterfaceLuid.Value].insert(
        AddressKey(reinterpret_cast<const SOCKADDR*>(&row.DestinationPrefix.Prefix)) + "/" +
        std::to_string(row.DestinationPrefix.PrefixLength) + ":" +
        AddressKey(reinterpret_cast<const SOCKADDR*>(&row.NextHop)) + ":" +
        std::to_string(row.Metric) + ":" + std::to_string(row.Protocol));
  }
  ::FreeMibTable(routes);

  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  DWORD status = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
    Check(size <= 16 * 1024 * 1024, "adapter snapshot exceeded bound");
    buffer.resize(size);
    status = ::GetAdaptersAddresses(AF_UNSPEC,
        GAA_FLAG_INCLUDE_ALL_INTERFACES | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  Check(status == NO_ERROR, "adapter enumeration failed");
  for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
       adapter = adapter->Next) {
    auto& copy = result.adapters[adapter->Luid.Value];
    if (adapter->FriendlyName) copy.alias = adapter->FriendlyName;
    GUID guid{};
    if (adapter->AdapterName && SUCCEEDED(::CLSIDFromString(Widen(adapter->AdapterName).c_str(), &guid)))
      copy.guid = guid;
    for (auto* dns = adapter->FirstDnsServerAddress; dns; dns = dns->Next)
      copy.dns.insert(AddressKey(dns->Address.lpSockaddr));
    for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next)
      copy.addresses.insert(AddressKey(address->Address.lpSockaddr) + "/" +
                            std::to_string(address->OnLinkPrefixLength));
    for (ADDRESS_FAMILY family : {AF_INET, AF_INET6}) {
      MIB_IPINTERFACE_ROW row{};
      row.Family = family;
      row.InterfaceLuid = adapter->Luid;
      const auto error = ::GetIpInterfaceEntry(&row);
      if (error == ERROR_NOT_FOUND) continue;
      Check(error == NO_ERROR, "interface configuration read failed");
      copy.interfaces.insert(std::to_string(family) + ":" + std::to_string(row.NlMtu) +
          ":" + std::to_string(row.Metric) + ":" + std::to_string(row.UseAutomaticMetric));
    }
  }
  NormalizeRetiredTestBinding(result, ReadDevicePresence);
  return result;
}

// Read only the product's two provider IDs and three sublayer IDs. Refuse to
// share them with an installed service; never sweep/delete a foreign owner.
std::array<GUID, 5> PolicyGuids() {
  const auto names = WfpPolicy::ObjectGuidsText();
  Check(names.size() == 5, "WFP ownership contract changed");
  std::array<GUID, 5> result{};
  for (size_t i = 0; i < names.size(); ++i) {
    const auto brace = names[i].find('{');
    Check(brace != std::string::npos &&
          SUCCEEDED(::CLSIDFromString(Widen(names[i].substr(brace)).c_str(), &result[i])),
          "cannot parse owned WFP GUID");
  }
  return result;
}

const GUID& LayerGuid(WfpLayer layer) {
  switch (layer) {
    case WfpLayer::ConnectV4: return FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    case WfpLayer::ConnectV6: return FWPM_LAYER_ALE_AUTH_CONNECT_V6;
    case WfpLayer::RecvAcceptV4: return FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V4;
    case WfpLayer::RecvAcceptV6: return FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6;
  }
  throw std::runtime_error("unexpected WFP layer");
}

const GUID& FieldGuid(WfpField field) {
  switch (field) {
    case WfpField::AppId: return FWPM_CONDITION_ALE_APP_ID;
    case WfpField::Flags: return FWPM_CONDITION_FLAGS;
    case WfpField::LocalInterface: return FWPM_CONDITION_IP_LOCAL_INTERFACE;
    case WfpField::Protocol: return FWPM_CONDITION_IP_PROTOCOL;
    case WfpField::LocalPort: return FWPM_CONDITION_IP_LOCAL_PORT;
    case WfpField::RemotePort: return FWPM_CONDITION_IP_REMOTE_PORT;
    case WfpField::LocalAddrV4:
    case WfpField::LocalAddrV6: return FWPM_CONDITION_IP_LOCAL_ADDRESS;
    case WfpField::RemoteAddrV4:
    case WfpField::RemoteAddrV6: return FWPM_CONDITION_IP_REMOTE_ADDRESS;
  }
  throw std::runtime_error("unexpected WFP field");
}

std::string ConditionKey(const FWPM_FILTER_CONDITION0& condition) {
  const auto& value = condition.conditionValue;
  std::string key = GuidKey(condition.fieldKey) + ":" + std::to_string(condition.matchType) + ":";
  switch (value.type) {
    case FWP_UINT8: return key + std::to_string(value.uint8);
    case FWP_UINT16: return key + std::to_string(value.uint16);
    case FWP_UINT32: return key + std::to_string(value.uint32);
    case FWP_UINT64: return key + std::to_string(*value.uint64);
    case FWP_BYTE_BLOB_TYPE: return key + Hex(value.byteBlob->data, value.byteBlob->size);
    case FWP_V4_ADDR_MASK:
      return key + std::to_string(value.v4AddrMask->addr) + "/" +
             std::to_string(value.v4AddrMask->mask);
    case FWP_V6_ADDR_MASK:
      return key + Hex(value.v6AddrMask->addr, 16) + "/" +
             std::to_string(value.v6AddrMask->prefixLength);
    default: throw std::runtime_error("unexpected owned WFP condition type");
  }
}

std::string ConditionKey(const WfpCondition& condition) {
  std::string key = GuidKey(FieldGuid(condition.field)) + ":" +
      std::to_string(condition.match == WfpMatch::FlagsAllSet ? FWP_MATCH_FLAGS_ALL_SET : FWP_MATCH_EQUAL) + ":";
  if (condition.field == WfpField::AppId) {
    FWP_BYTE_BLOB* blob = nullptr;
    Check(::FwpmGetAppIdFromFileName0(condition.app_path.c_str(), &blob) == ERROR_SUCCESS && blob,
          "cannot resolve expected test image identity");
    key += Hex(blob->data, blob->size);
    ::FwpmFreeMemory0(reinterpret_cast<void**>(&blob));
    return key;
  }
  if (condition.field == WfpField::LocalAddrV4 || condition.field == WfpField::RemoteAddrV4) {
    const uint32_t mask = condition.v4_prefix == 0 ? 0 : 0xffffffffu << (32 - condition.v4_prefix);
    return key + std::to_string(condition.v4_addr) + "/" + std::to_string(mask);
  }
  if (condition.field == WfpField::LocalAddrV6 || condition.field == WfpField::RemoteAddrV6)
    return key + Hex(condition.v6_addr, 16) + "/" + std::to_string(condition.v6_prefix);
  return key + std::to_string(condition.number);
}

std::string FilterKey(std::string name, const GUID& layer, const GUID& sublayer,
                      FWP_ACTION_TYPE action, std::vector<std::string> conditions) {
  std::sort(conditions.begin(), conditions.end());
  std::string key = name + ":" + GuidKey(layer) + ":" + GuidKey(sublayer) + ":" + std::to_string(action);
  for (const auto& condition : conditions) key += "|" + condition;
  return key;
}

struct PolicySnapshot {
  size_t objects = 0;
  std::set<std::string> objectKeys;
  std::multiset<std::string> filters;
  bool operator==(const PolicySnapshot&) const = default;
};

struct Engine {
  HANDLE value = nullptr;
  HANDLE enumeration = nullptr;
  Engine() {
    Check(::FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr, &value) == ERROR_SUCCESS,
          "cannot inspect BFE");
  }
  ~Engine() {
    if (enumeration) ::FwpmFilterDestroyEnumHandle0(value, enumeration);
    if (value) ::FwpmEngineClose0(value);
  }
};

PolicySnapshot ReadPolicy() {
  Engine engine;
  const auto guids = PolicyGuids();
  PolicySnapshot result;
  for (size_t i = 0; i < guids.size(); ++i) {
    DWORD status;
    void* object = nullptr;
    if (i < 2) status = ::FwpmProviderGetByKey0(engine.value, &guids[i], reinterpret_cast<FWPM_PROVIDER0**>(&object));
    else status = ::FwpmSubLayerGetByKey0(engine.value, &guids[i], reinterpret_cast<FWPM_SUBLAYER0**>(&object));
    Check(status == ERROR_SUCCESS || status == FWP_E_PROVIDER_NOT_FOUND || status == FWP_E_SUBLAYER_NOT_FOUND,
          "owned WFP object read failed");
    if (status == ERROR_SUCCESS) {
      ++result.objects;
      result.objectKeys.insert(GuidKey(guids[i]));
      ::FwpmFreeMemory0(&object);
    }
  }
  Check(::FwpmFilterCreateEnumHandle0(engine.value, nullptr, &engine.enumeration) == ERROR_SUCCESS,
        "WFP enumeration setup failed");
  size_t total = 0;
  for (;;) {
    FWPM_FILTER0** filters = nullptr;
    UINT32 count = 0;
    Check(::FwpmFilterEnum0(engine.value, engine.enumeration, 256, &filters, &count) == ERROR_SUCCESS,
          "WFP enumeration failed");
    total += count;
    if (total > 65536) {
      ::FwpmFreeMemory0(reinterpret_cast<void**>(&filters));
      throw std::runtime_error("WFP snapshot exceeded bound");
    }
    for (UINT32 i = 0; i < count; ++i) {
      const auto& filter = *filters[i];
      if (!filter.providerKey || (*filter.providerKey != guids[0] && *filter.providerKey != guids[1])) continue;
      std::vector<std::string> conditions;
      for (UINT32 c = 0; c < filter.numFilterConditions; ++c)
        conditions.push_back(ConditionKey(filter.filterCondition[c]));
      result.filters.insert(FilterKey(Narrow(filter.displayData.name ? filter.displayData.name : L""),
          filter.layerKey, filter.subLayerKey, filter.action.type, std::move(conditions)));
    }
    ::FwpmFreeMemory0(reinterpret_cast<void**>(&filters));
    if (count == 0) break;
  }
  return result;
}

// Raw baseline values stay in restricted local artifacts, never stdout. Files
// are bounded and create-new, so a retry cannot overwrite a prior run's evidence.
void WriteArtifact(const std::string& name, const std::string& contents) {
  Check(contents.size() <= 4 * 1024 * 1024, "native diagnostic artifact exceeded bound");
  const auto path = std::filesystem::path(ExePath()).parent_path() / Widen(name);
  Handle file(::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
  Check(file.value != INVALID_HANDLE_VALUE, "cannot create native diagnostic artifact");
  DWORD written = 0;
  Check(::WriteFile(file.value, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
        written == contents.size() && ::FlushFileBuffers(file.value), "cannot persist native diagnostic artifact");
}

void AppendLine(std::string& text, const std::string& line) {
  Check(line.size() < 64 * 1024 && text.size() + line.size() + 1 <= 4 * 1024 * 1024,
        "native snapshot exceeded artifact bound");
  text += line;
  text += '\n';
}

void SaveNetwork(const std::string& name, const NetworkSnapshot& snapshot) {
  std::string text;
  for (const auto& [luid, routes] : snapshot.routes)
    for (const auto& route : routes) AppendLine(text, "route\t" + std::to_string(luid) + "\t" + route);
  for (const auto& [luid, adapter] : snapshot.adapters) {
    const auto owner = std::to_string(luid) + "\t";
    AppendLine(text, "adapter\t" + owner + Hex(adapter.alias.data(), adapter.alias.size() * sizeof(wchar_t)) +
               "\t" + GuidKey(adapter.guid));
    for (const auto& dns : adapter.dns) AppendLine(text, "dns\t" + owner + dns);
    for (const auto& address : adapter.addresses) AppendLine(text, "address\t" + owner + address);
    for (const auto& settings : adapter.interfaces) AppendLine(text, "interface\t" + owner + settings);
  }
  for (const auto& [luid, adapter] : snapshot.retiredBindings)
    AppendLine(text, "retired-binding\t" + std::to_string(luid) + "\t" +
        Hex(adapter.alias.data(), adapter.alias.size() * sizeof(wchar_t)) + "\t" + GuidKey(adapter.guid) +
        "\tpresent_device=0 ip_rows=0 routes=0 dns=0 addresses=0");
  for (const auto& [luid, state] : snapshot.testDevicePresence)
    AppendLine(text, "test-device-presence\t" + std::to_string(luid) + "\t" +
        (state == DevicePresence::Absent ? "absent" : state == DevicePresence::Present ? "present" : "unknown"));
  WriteArtifact(name + "-network.tsv", text);
}

void SavePolicy(const std::string& name, const PolicySnapshot& snapshot) {
  std::string text;
  AppendLine(text, "owned_objects\t" + std::to_string(snapshot.objects));
  for (const auto& key : snapshot.objectKeys) AppendLine(text, "object\t" + key);
  for (const auto& filter : snapshot.filters) AppendLine(text, "filter\t" + Hex(filter.data(), filter.size()));
  WriteArtifact(name + "-policy.tsv", text);
}

size_t RouteCount(const NetworkSnapshot& snapshot) {
  size_t count = 0;
  for (const auto& [luid, routes] : snapshot.routes) count += routes.size();
  return count;
}

// Count differences by the same fields the strict equality assertion uses.
// These counts explain a failure; none are exclusions from baseline equality.
std::string NetworkDifferences(const NetworkSnapshot& before, const NetworkSnapshot& after) {
  size_t routesAdded = 0, routesRemoved = 0, adaptersAdded = 0, adaptersRemoved = 0;
  size_t aliasesChanged = 0, dnsChanged = 0, addressesChanged = 0, interfacesChanged = 0;
  for (const auto& [luid, routes] : after.routes) {
    const auto prior = before.routes.find(luid);
    for (const auto& route : routes)
      if (prior == before.routes.end() || !prior->second.contains(route)) ++routesAdded;
  }
  for (const auto& [luid, routes] : before.routes) {
    const auto current = after.routes.find(luid);
    for (const auto& route : routes)
      if (current == after.routes.end() || !current->second.contains(route)) ++routesRemoved;
  }
  for (const auto& [luid, adapter] : after.adapters) {
    const auto prior = before.adapters.find(luid);
    if (prior == before.adapters.end()) {
      ++adaptersAdded;
      continue;
    }
    aliasesChanged += adapter.alias != prior->second.alias;
    dnsChanged += adapter.dns != prior->second.dns;
    addressesChanged += adapter.addresses != prior->second.addresses;
    interfacesChanged += adapter.interfaces != prior->second.interfaces;
  }
  for (const auto& [luid, adapter] : before.adapters)
    if (!after.adapters.contains(luid)) ++adaptersRemoved;
  return "routes_added=" + std::to_string(routesAdded) + " routes_removed=" + std::to_string(routesRemoved) +
      " adapters_added=" + std::to_string(adaptersAdded) + " adapters_removed=" + std::to_string(adaptersRemoved) +
      " aliases_changed=" + std::to_string(aliasesChanged) + " dns_changed=" + std::to_string(dnsChanged) +
      " addresses_changed=" + std::to_string(addressesChanged) + " interfaces_changed=" + std::to_string(interfacesChanged);
}

void CheckPolicy(WfpState state, const WfpConfig& config) {
  const auto actual = ReadPolicy();
  const auto guids = PolicyGuids();
  std::multiset<std::string> expected;
  for (const auto& filter : BuildFilterSet(state, config)) {
    std::vector<std::string> conditions;
    for (const auto& condition : filter.conditions) conditions.push_back(ConditionKey(condition));
    const size_t sublayer = filter.sublayer == WfpSublayer::Baseline ? 2 :
                            filter.sublayer == WfpSublayer::Dns ? 3 : 4;
    expected.insert(FilterKey(filter.name, LayerGuid(filter.layer), guids[sublayer],
        filter.block ? FWP_ACTION_BLOCK : FWP_ACTION_PERMIT, std::move(conditions)));
  }
  Check(actual.filters == expected, "actual BFE filters differ from requested policy");
  Check(actual.objects == (state == WfpState::Off ? 0u : 3u), "unexpected owned BFE object count");
}

void CheckPhysicalUnchanged(NetworkSnapshot snapshot, const NetworkSnapshot& baseline, NET_LUID tun) {
  snapshot.adapters.erase(tun.Value);
  snapshot.routes.erase(tun.Value);
  Check(snapshot == baseline, "physical routes, addresses, DNS, or interface settings changed");
}

// Inspect every capture prefix, not just a count or NetworkConfig's booleans.
void CheckCaptureRoutes(NET_LUID tun, bool v4, bool v6) {
  MIB_IPFORWARD_TABLE2* table = nullptr;
  Check(::GetIpForwardTable2(AF_UNSPEC, &table) == NO_ERROR, "capture route read failed");
  std::set<std::string> actual;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const auto& row = table->Table[i];
    if (row.InterfaceLuid.Value == tun.Value)
      actual.insert(AddressKey(reinterpret_cast<const SOCKADDR*>(&row.DestinationPrefix.Prefix)) + "/" +
                    std::to_string(row.DestinationPrefix.PrefixLength));
  }
  ::FreeMibTable(table);
  for (const auto prefix : net::kTunCaptureV4) {
    SOCKADDR_IN address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(prefix.network);
    const auto key = AddressKey(reinterpret_cast<const SOCKADDR*>(&address)) + "/" + std::to_string(prefix.prefix);
    Check(actual.contains(key) == v4, "IPv4 capture prefix differs from expected state");
  }
  for (const auto prefix : net::kTunCaptureV6) {
    SOCKADDR_IN6 address{};
    address.sin6_family = AF_INET6;
    const auto bytes = prefix.Bytes();
    std::memcpy(&address.sin6_addr, bytes.data(), bytes.size());
    const auto key = AddressKey(reinterpret_cast<const SOCKADDR*>(&address)) + "/" + std::to_string(prefix.prefix);
    Check(actual.contains(key) == v6, "IPv6 capture prefix differs from expected state");
  }
}

// A denied connect is a kernel policy discriminator. A timeout, unreachable
// route, or in-progress connect is NOT evidence that the Internet is reachable.
bool ConnectDenied() {
  SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Check(socket != INVALID_SOCKET, "test socket creation failed");
  u_long nonblocking = 1;
  if (::ioctlsocket(socket, FIONBIO, &nonblocking) != 0) {
    ::closesocket(socket);
    throw std::runtime_error("cannot bound test socket");
  }
  SOCKADDR_IN address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(9);
  ::InetPtonA(AF_INET, "203.0.113.254", &address.sin_addr);
  int error = 0;
  if (::connect(socket, reinterpret_cast<SOCKADDR*>(&address), sizeof(address)) == SOCKET_ERROR)
    error = ::WSAGetLastError();
  if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) {
    fd_set writeSet, errorSet;
    FD_ZERO(&writeSet); FD_ZERO(&errorSet);
    FD_SET(socket, &writeSet); FD_SET(socket, &errorSet);
    timeval timeout{2, 0};
    const int ready = ::select(0, nullptr, &writeSet, &errorSet, &timeout);
    if (ready == SOCKET_ERROR) error = ::WSAGetLastError();
    else if (ready > 0) {
      int size = sizeof(error);
      if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0)
        error = ::WSAGetLastError();
    }
  }
  ::closesocket(socket);
  return error == WSAEACCES;
}

void Preflight() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  Check(::AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
      DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &administrators), "cannot check elevation");
  BOOL member = FALSE;
  const BOOL checked = ::CheckTokenMembership(nullptr, administrators, &member);
  ::FreeSid(administrators);
  Check(checked && member, "requires elevated disposable VM");
  SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  Check(manager != nullptr, "cannot inspect service ownership");
  SC_HANDLE service = ::OpenServiceW(manager, ids::kServiceName, SERVICE_QUERY_STATUS);
  const DWORD error = ::GetLastError();
  if (service) ::CloseServiceHandle(service);
  ::CloseServiceHandle(manager);
  Check(!service && error == ERROR_SERVICE_DOES_NOT_EXIST, "refusing VM with installed URnetwork service");
  const auto policy = ReadPolicy();
  Check(policy.objects == 0 && policy.filters.empty(), "refusing pre-existing URnetwork WFP ownership");
  for (const auto& [luid, adapter] : ReadNetwork().adapters)
    Check(adapter.alias != kAdapterName && adapter.alias != ids::kTunAdapterName,
          "refusing pre-existing tunnel adapter");
}

// A native ring is created, but the SDK PacketPump is deliberately absent.
// The prepare marker tests transaction ordering, not packet forwarding.
struct Machine {
  NetworkSnapshot baseline = ReadNetwork();
  std::unique_ptr<Wintun> api;
  std::unique_ptr<WintunAdapter> adapter;
  std::unique_ptr<NetworkConfig> network;
  WfpPolicy policy;
  TunnelNetworkSettings settings;
  WfpConfig config;
  bool armed = false;
  bool prepared = false;
  int applies = 0;
  int rollbacks = 0;

  explicit Machine(bool dual) {
    api = Wintun::Load(std::filesystem::path(ExePath()).parent_path() / L"wintun.dll");
    Check(api != nullptr, "cannot load staged Wintun DLL");
    adapter = WintunAdapter::Create(*api, kAdapterName, kAdapterGuid, WINTUN_MIN_RING_CAPACITY);
    Check(adapter != nullptr, "cannot create native test adapter and ring");
    network = std::make_unique<NetworkConfig>(adapter->Luid());
    settings.local_address_v4 = "192.0.2.1";
    settings.dns_servers_v4 = {"198.51.100.53"};
    if (dual) {
      settings.local_address_v6 = "2001:db8:1234::1";
      settings.dns_servers_v6 = {"2001:db8:5678::53"};
    }
    config.tun_luid = adapter->Luid().Value;
    config.tunnel_resolvers_v4 = settings.dns_servers_v4;
    config.tunnel_resolvers_v6 = settings.dns_servers_v6;
    config.tunnel_ipv6 = dual;
    config.host_resolvers_v4 = {"203.0.113.53"};
    config.service_image_path = ExePath();
    CheckPhysicalUnchanged(ReadNetwork(), baseline, adapter->Luid());
  }

  ~Machine() {
    if (network) network->Revert();
    policy.Revert();
    adapter.reset();
  }

  void CheckNoCapture() {
    const auto snapshot = ReadNetwork();
    CheckPhysicalUnchanged(snapshot, baseline, adapter->Luid());
    CheckCaptureRoutes(adapter->Luid(), false, false);
    const auto& tun = snapshot.adapters.at(adapter->Luid().Value);
    Check(!tun.dns.contains(LiteralKey("198.51.100.53", AF_INET)) &&
          !tun.dns.contains(LiteralKey("2001:db8:5678::53", AF_INET6)), "tunnel DNS survived rollback");
    Check(!tun.addresses.contains(LiteralKey("192.0.2.1", AF_INET) + "/24") &&
          !tun.addresses.contains(LiteralKey("2001:db8:1234::1", AF_INET6) + "/64"),
          "tunnel address survived rollback");
    CheckPolicy(armed ? WfpState::Armed : WfpState::Off, config);
  }

  void CheckActive() {
    const auto snapshot = ReadNetwork();
    CheckPhysicalUnchanged(snapshot, baseline, adapter->Luid());
    CheckCaptureRoutes(adapter->Luid(), true, settings.HasIpv6());
    const auto& tun = snapshot.adapters.at(adapter->Luid().Value);
    Check(tun.dns.contains(LiteralKey("198.51.100.53", AF_INET)), "actual tunnel DNS missing");
    Check(tun.addresses.contains(LiteralKey("192.0.2.1", AF_INET) + "/24"), "actual tunnel address missing");
    if (settings.HasIpv6()) {
      Check(tun.dns.contains(LiteralKey("2001:db8:5678::53", AF_INET6)), "actual IPv6 tunnel DNS missing");
      Check(tun.addresses.contains(LiteralKey("2001:db8:1234::1", AF_INET6) + "/64"),
            "actual IPv6 tunnel address missing");
    }
    CheckPolicy(WfpState::Connected, config);
  }

  bool Apply(CaptureStage stage) {
    ++applies;
    if (stage == CaptureStage::Prepare) {
      Check(adapter->ReadWaitEvent() != nullptr, "native ring wait event missing");
      prepared = true;
    } else {
      Check(prepared, "OS capture ran before preparation");
      if (stage == CaptureStage::Firewall)
        Check(policy.Apply(WfpState::Connecting, config), "native connecting WFP failed");
      if (stage == CaptureStage::Network)
        Check(network->Apply(settings) && network->DnsApplied(), "native routes or DNS failed");
      if (stage == CaptureStage::Connected)
        Check(policy.Apply(WfpState::Connected, config), "native connected WFP failed");
    }
    return true;
  }

  void Rollback() {
    ++rollbacks;
    network->Revert();
    if (armed) Check(policy.Apply(WfpState::Armed, config), "native armed rollback failed");
    else policy.Revert();
    prepared = false;
  }
};

// Diagnostic-only: preserve explicit API settings and the effective GAA view.
// Getter flags remain zero per its contract; this is not an IPv6 getter claim.
void SaveDnsDiagnostic(Machine& machine, const std::string& stage) {
  const auto network = ReadNetwork();
  SaveNetwork("dns-" + stage, network);
  const auto& tun = network.adapters.at(machine.adapter->Luid().Value);
  Check(tun.alias == kAdapterName && tun.guid == kAdapterGuid, "DNS diagnostic lost its exact synthetic adapter");
  const bool effective4 = tun.dns.contains(LiteralKey("198.51.100.53", AF_INET));
  const bool effective6 = tun.dns.contains(LiteralKey("2001:db8:5678::53", AF_INET6));
  DNS_INTERFACE_SETTINGS settings{};
  settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
  const DWORD status = ::GetInterfaceDnsSettings(kAdapterGuid, &settings);
  std::string text = "get_status=" + std::to_string(status) + "\neffective_test_v4=" +
      std::to_string(effective4) + "\neffective_test_v6=" + std::to_string(effective6) + "\n";
  if (status == NO_ERROR) {
    try {
      AppendLine(text, "flags=" + std::to_string(settings.Flags));
      auto value = [&](const char* field, const wchar_t* pointer) {
        if (!pointer) {
          AppendLine(text, std::string(field) + "=null");
          return;
        }
        size_t length = 0;
        while (length < 4096 && pointer[length]) ++length;
        Check(length < 4096, "DNS diagnostic field exceeded bound");
        AppendLine(text, std::string(field) + "=utf16:" + Hex(pointer, length * sizeof(wchar_t)));
      };
      value("nameserver", settings.NameServer);
      value("search_list", settings.SearchList);
      value("profile_nameserver", settings.ProfileNameServer);
    } catch (...) {
      ::FreeInterfaceDnsSettings(&settings);
      throw;
    }
    ::FreeInterfaceDnsSettings(&settings);
  }
  WriteArtifact("dns-" + stage + "-settings.txt", text);
  std::printf("DNS diagnostic stage=%s get_status=%lu effective_test_v4=%d effective_test_v6=%d\n",
              stage.c_str(), status, effective4, effective6);
  std::fflush(stdout);
}

// Never part of the normal acceptance cases. Compare the original null payload
// with explicit empty strings on this child's own test adapter only, retaining
// every API result and state. The parent still verifies complete final cleanup.
void RunDnsClearDiagnostic(Machine& machine) {
  machine.settings.dns_search = "native-dns-test.example";
  SaveDnsDiagnostic(machine, "initial");
  for (auto stage : {CaptureStage::Prepare, CaptureStage::Firewall, CaptureStage::Network, CaptureStage::Connected})
    machine.Apply(stage);
  machine.CheckActive();
  SaveDnsDiagnostic(machine, "applied");
  machine.Rollback();
  SaveDnsDiagnostic(machine, "production-revert-0s");
  for (const auto elapsed : {1, 5, 10}) {
    ::Sleep(elapsed == 1 ? 1000 : elapsed == 5 ? 4000 : 5000);
    SaveDnsDiagnostic(machine, "production-revert-" + std::to_string(elapsed) + "s");
  }
  for (const bool empty : {false, true}) {
    for (const bool ipv6 : {false, true}) {
      const auto network = ReadNetwork();
      const auto& tun = network.adapters.at(machine.adapter->Luid().Value);
      Check(tun.alias == kAdapterName && tun.guid == kAdapterGuid, "refusing DNS comparator outside exact test adapter");
      wchar_t blank[] = L"";
      DNS_INTERFACE_SETTINGS settings{};
      settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
      settings.Flags = DNS_SETTING_NAMESERVER | DNS_SETTING_SEARCHLIST | (ipv6 ? DNS_SETTING_IPV6 : 0);
      settings.NameServer = empty ? blank : nullptr;
      settings.SearchList = empty ? blank : nullptr;
      const DWORD status = ::SetInterfaceDnsSettings(kAdapterGuid, &settings);
      const std::string stage = std::string(empty ? "empty" : "null") + (ipv6 ? "-v6" : "-v4");
      WriteArtifact("dns-" + stage + "-set-result.txt", "set_status=" + std::to_string(status) + "\n");
      std::printf("DNS diagnostic comparator=%s set_status=%lu\n", stage.c_str(), status);
      std::fflush(stdout);
      SaveDnsDiagnostic(machine, stage);
    }
  }
  for (const auto elapsed : {1, 5, 10}) {
    ::Sleep(elapsed == 1 ? 1000 : elapsed == 5 ? 4000 : 5000);
    SaveDnsDiagnostic(machine, "empty-revert-" + std::to_string(elapsed) + "s");
  }
  // Deliberately retain the normal assertion; observation is not acceptance.
  machine.CheckNoCapture();
}

constexpr char kDnsDiagnosticCase[] = "diagnostic-dns-clear";

const std::vector<std::string> kCases{
    "waiting-native", "armed-policy", "active-v4", "active-dual",
    "fail-prepare", "fail-firewall", "fail-network", "fail-connected", "throw-network",
    "expired-before", "superseded-before", "cancelled-before", "network-before",
    "expired-network", "superseded-network", "cancelled-network", "cancelled-network-armed",
    "invalid-network-settings", "crash-active"};

void RunCase(const std::string& name, const std::wstring& readyEvent) {
  Machine machine(name == "active-dual" || name == "crash-active");
  if (name == kDnsDiagnosticCase) {
    RunDnsClearDiagnostic(machine);
    return;
  }
  int64_t now = 10000;
  CaptureReadiness readiness([&] { return now; });
  if (name == "waiting-native" || name == "armed-policy") {
    if (name == "armed-policy") {
      // The harness is the UI, NOT the service exemption, for this discriminator.
      wchar_t systemDirectory[MAX_PATH]{};
      Check(::GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0, "cannot find synthetic service image");
      machine.config.service_image_path = std::filesystem::path(systemDirectory) / L"cmd.exe";
      machine.config.app_image_path = ExePath();
      Check(!ConnectDenied(), "baseline socket already access-denied; enforcement control unavailable");
      machine.armed = true;
      Check(machine.policy.Apply(WfpState::Armed, machine.config), "cannot arm explicit policy");
      Check(ConnectDenied(), "Armed did not deny unpermitted test image");
    }
    const auto result = ApplyCapture(readiness, {}, [&](CaptureStage stage) { return machine.Apply(stage); },
                                    [&] { machine.Rollback(); });
    Check(result == CaptureResult::Waiting && machine.applies == 0, "unproved readiness changed the OS");
    machine.CheckNoCapture();
    if (machine.armed) {
      Check(machine.policy.Apply(WfpState::Connecting, machine.config), "cannot open exact UI bootstrap permit");
      CheckPolicy(WfpState::Connecting, machine.config);
      Check(!ConnectDenied(), "Connecting denied exact UI image");
      CheckCaptureRoutes(machine.adapter->Luid(), false, false);
      Check(machine.policy.Apply(WfpState::Armed, machine.config), "cannot restore Armed");
      Check(ConnectDenied(), "Armed retained the Connecting UI exception");
      machine.policy.Revert();
      machine.armed = false;
      machine.CheckNoCapture();
      Check(!ConnectDenied(), "disarm retained access denial");
    }
    return;
  }
  const CaptureWindow window{.generation = 1, .added = 1};
  readiness.CompleteSample(readiness.BeginSample(), window, window, 1);
  const auto ticket = readiness.Ready();
  Check(ticket.has_value(), "synthetic proof was not accepted");
  if (name == "expired-before") now += 8001;
  if (name == "superseded-before") readiness.Invalidate(2);
  if (name == "cancelled-before") readiness.Cancel();
  if (name == "network-before") readiness.NetworkChanged(now);
  machine.armed = name == "cancelled-network-armed";
  if (machine.armed) Check(machine.policy.Apply(WfpState::Armed, machine.config), "cannot arm before capture");
  CaptureResult result = CaptureResult::Waiting;
  bool threw = false;
  try {
    result = ApplyCapture(readiness, *ticket, [&](CaptureStage stage) {
      if (name == "invalid-network-settings" && stage == CaptureStage::Network) {
        auto invalid = machine.settings;
        invalid.local_address_v4 = "malformed-native-test.example";
        Check(!machine.network->Apply(invalid), "invalid configuration reached native capture");
        return false;
      }
      machine.Apply(stage);
      const bool injectedFailure =
          (name == "fail-prepare" && stage == CaptureStage::Prepare) ||
          (name == "fail-firewall" && stage == CaptureStage::Firewall) ||
          (name == "fail-network" && stage == CaptureStage::Network) ||
          (name == "fail-connected" && stage == CaptureStage::Connected);
      if (stage == CaptureStage::Network) {
        if (name == "throw-network") throw std::runtime_error("synthetic stage exception");
        if (name == "expired-network") now += 8001;
        if (name == "superseded-network") readiness.Invalidate(2);
        if (name == "cancelled-network" || name == "cancelled-network-armed") readiness.Cancel();
      }
      return !injectedFailure;
    }, [&] { machine.Rollback(); });
  } catch (const std::runtime_error& error) {
    if (name != "throw-network" || std::string(error.what()) != "synthetic stage exception") throw;
    threw = true;
  }
  if (name == "active-v4" || name == "active-dual" || name == "crash-active") {
    Check(result == CaptureResult::Active && machine.applies == 4, "qualified capture did not commit");
    machine.CheckActive();
    if (name == "crash-active") {
      Handle event(::OpenEventW(EVENT_MODIFY_STATE, FALSE, readyEvent.c_str()));
      Check(event.value && ::SetEvent(event.value), "cannot publish crash boundary");
      // Only the parent kills this bounded, job-owned child; destructors must not run.
      ::Sleep(INFINITE);
    }
    machine.Rollback();
  } else {
    const bool before = name.ends_with("-before");
    const bool failure = name.starts_with("fail-") || name == "invalid-network-settings";
    Check(threw || result == (failure ? CaptureResult::Halted : CaptureResult::Waiting), "incorrect transaction outcome");
    Check(machine.rollbacks == (before ? 0 : 1), "incorrect rollback ownership");
    if (before) Check(machine.applies == 0, "stale ticket reached an OS stage");
  }
  machine.CheckNoCapture();
}

// The OS may retire a Wintun device asynchronously. This bounded acceptance
// wait is cleanup verification, not the ordering mechanism of the regression.
void WaitRestored(const NetworkSnapshot& baseline, const std::string& name) {
  const ULONGLONG deadline = ::GetTickCount64() + 10000;
  for (;;) {
    const auto policy = ReadPolicy();
    const auto network = ReadNetwork();
    const bool restored = network == baseline && policy.objects == 0 && policy.filters.empty();
    if (restored || ::GetTickCount64() >= deadline) {
      SaveNetwork("after-" + name, network);
      SavePolicy("after-" + name, policy);
      const auto differences = NetworkDifferences(baseline, network);
      const auto verdict = "restored=" + std::to_string(restored) + " owned_objects=" + std::to_string(policy.objects) +
          " owned_filters=" + std::to_string(policy.filters.size()) +
          " retired_test_bindings=" + std::to_string(network.retiredBindings.size()) + " " + differences;
      WriteArtifact("after-" + name + "-verdict.txt", verdict + "\n");
      std::printf("CLEANUP %s %s\n", name.c_str(), verdict.c_str());
      std::fflush(stdout);
      Check(restored, "baseline not restored; discard disposable VM overlay");
      return;
    }
    ::Sleep(100);
  }
}

void ReportChild(const std::string& name, HANDLE child, DWORD wait, bool terminated, bool boundaryReached) {
  DWORD exit = STILL_ACTIVE;
  Check(::GetExitCodeProcess(child, &exit) && exit != STILL_ACTIVE, "cannot read terminal native child exit");
  const auto status = "exit=" + std::to_string(exit) + " wait=" + std::to_string(wait) +
      " terminated=" + std::to_string(terminated) + " boundary_reached=" + std::to_string(boundaryReached);
  // Report terminal status BEFORE any artifact/cleanup failure can mask it.
  std::printf("CHILD %s %s\n", name.c_str(), status.c_str());
  std::fflush(stdout);
  WriteArtifact("child-" + name + ".txt", status + "\n");
}

void RunChild(const std::string& name, const NetworkSnapshot& baseline) {
  Handle job(::CreateJobObjectW(nullptr, nullptr));
  Check(job.value != nullptr, "cannot own test child");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  Check(::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
        "cannot bound child lifetime");
  const std::wstring eventName = L"Local\\UrnNativeCaptureReady-" + std::to_wstring(::GetCurrentProcessId());
  Handle event(::CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
  Check(event.value != nullptr && ::GetLastError() != ERROR_ALREADY_EXISTS, "crash boundary event already owned");
  std::wstring command = L"\"" + ExePath() + L"\" --disposable-vm --case=" + Widen(name) +
      L" --ready-event=" + eventName;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  std::printf("RUN %s\n", name.c_str());
  std::fflush(stdout);
  Check(::CreateProcessW(ExePath().c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process), "cannot launch native test child");
  Handle child(process.hProcess), thread(process.hThread);
  if (!::AssignProcessToJobObject(job.value, child.value)) {
    ::TerminateProcess(child.value, 1);
    Check(::WaitForSingleObject(child.value, 10000) == WAIT_OBJECT_0, "unassigned child did not stop");
    ReportChild(name, child.value, WAIT_FAILED, true, false);
    WaitRestored(baseline, name);
    throw std::runtime_error("cannot assign native child to cleanup job");
  }
  bool ok = false;
  bool boundaryReached = false;
  DWORD wait = WAIT_FAILED;
  std::exception_ptr failure;
  try {
    Check(::ResumeThread(thread.value) != static_cast<DWORD>(-1), "cannot start test child");
    if (name == "crash-active") {
      HANDLE handles[] = {event.value, child.value};
      wait = ::WaitForMultipleObjects(2, handles, FALSE, kChildBudgetMillis);
      if (wait == WAIT_OBJECT_0) {
        boundaryReached = true;
        // The child checked actual dual-stack routes/DNS/BFE before signaling.
        const auto policy = ReadPolicy();
        Check(policy.objects == 3 && !policy.filters.empty(), "crash boundary had no native WFP state");
        ok = true;
      }
    } else {
      wait = ::WaitForSingleObject(child.value, kChildBudgetMillis);
      DWORD exit = 1;
      if (wait == WAIT_OBJECT_0) {
        Check(::GetExitCodeProcess(child.value, &exit), "cannot read test child exit");
        ok = exit == 0;
      }
    }
  } catch (...) {
    failure = std::current_exception();
  }
  const bool terminated = ::WaitForSingleObject(child.value, 0) != WAIT_OBJECT_0;
  if (terminated)
    Check(::TerminateJobObject(job.value, 23), "cannot terminate owned native test child");
  Check(::WaitForSingleObject(child.value, 10000) == WAIT_OBJECT_0, "test child cleanup did not finish");
  ReportChild(name, child.value, wait, terminated, boundaryReached);
  WaitRestored(baseline, name);
  if (failure) std::rethrow_exception(failure);
  Check(ok, "native case failed or exceeded its deadline");
  std::printf("PASS %s baseline-restored\n", name.c_str());
  std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  bool acknowledged = false;
  bool snapshotSelftest = false;
  bool dnsDiagnostic = false;
  std::string name;
  std::wstring readyEvent;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--disposable-vm") acknowledged = true;
      else if (arg == L"--snapshot-selftest") snapshotSelftest = true;
      else if (arg == L"--dns-clear-diagnostic") dnsDiagnostic = true;
      else if (arg.starts_with(L"--case=")) name = Narrow(arg.substr(7));
      else if (arg.starts_with(L"--ready-event=")) readyEvent = arg.substr(14);
      else throw std::runtime_error("unknown native test argument");
    }
    Check(acknowledged, "requires --disposable-vm; do not run on a workstation");
    Check(name.empty() || name == kDnsDiagnosticCase || std::find(kCases.begin(), kCases.end(), name) != kCases.end(),
          "unknown native case");
    Check(!snapshotSelftest || (name.empty() && readyEvent.empty() && !dnsDiagnostic),
          "snapshot selftest cannot select an OS case");
    Check(!dnsDiagnostic || (name.empty() && readyEvent.empty()), "DNS diagnostic must be parent-owned");
    if (name.empty()) TestRetiredBindingDiscriminator();
    if (snapshotSelftest) return 0;
    WSADATA winsock{};
    Check(::WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "Winsock startup failed");
    Preflight();
    if (!name.empty()) {
      Check(!readyEvent.empty(), "child case requires parent-owned boundary");
      RunCase(name, readyEvent);
    } else {
      Handle mutex(::CreateMutexW(nullptr, FALSE, L"Local\\UrnNativeCaptureAcceptance"));
      Check(mutex.value && ::GetLastError() != ERROR_ALREADY_EXISTS, "another native acceptance owns this VM");
      const auto baseline = ReadNetwork();
      SaveNetwork("baseline", baseline);
      SavePolicy("baseline", ReadPolicy());
      std::printf("BASELINE adapters=%zu routes=%zu\n", baseline.adapters.size(), RouteCount(baseline));
      std::fflush(stdout);
      if (dnsDiagnostic) {
        RunChild(kDnsDiagnosticCase, baseline);
        std::printf("PASS diagnostic-only DNS comparison completed; NOT native OS acceptance\n");
      } else {
        for (const auto& test : kCases) RunChild(test, baseline);
        std::printf("PASS native OS acceptance: %zu cases; injected proof, no provider validation\n", kCases.size());
      }
    }
    ::WSACleanup();
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL native acceptance: %s\n", error.what());
    return 1;
  }
}
