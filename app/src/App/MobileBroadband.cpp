// SPDX-License-Identifier: MPL-2.0
// Built without the precompiled header (App.vcxproj): plain Win32 and COM, no
// WinRT.
#include "MobileBroadband.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// winsock2 before the IP helpers, as EgressMonitor.h has it.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
// WIN32_LEAN_AND_MEAN leaves COM out of windows.h.
#include <objbase.h>
#include <oleauto.h>
#include <mbnapi.h>
#include <wrl/client.h>

#include "Log.h"
#include "Strings.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace urnw {
namespace {

// A Windows constant as NetworkCountry.h types it, for the checks below: the
// values that header restates so it can stay free of Windows headers.
constexpr uint32_t U32(auto value) { return static_cast<uint32_t>(value); }
static_assert(netcountry::kIfTypeWwanPp == U32(IF_TYPE_WWANPP));
static_assert(netcountry::kIfTypeWwanPp2 == U32(IF_TYPE_WWANPP2));
static_assert(netcountry::kPhysicalMediumWirelessWan == U32(NdisPhysicalMediumWirelessWan));
static_assert(netcountry::kRegisterStateHome == U32(MBN_REGISTER_STATE_HOME));
static_assert(netcountry::kRegisterStateRoaming == U32(MBN_REGISTER_STATE_ROAMING));
static_assert(netcountry::kRegisterStatePartner == U32(MBN_REGISTER_STATE_PARTNER));
static_assert((netcountry::kDataClass3gpp & U32(MBN_DATA_CLASS_GPRS)) != 0 &&
              (netcountry::kDataClass3gpp & U32(MBN_DATA_CLASS_UMTS)) != 0 &&
              (netcountry::kDataClass3gpp & U32(MBN_DATA_CLASS_LTE)) != 0);
static_assert((netcountry::kDataClass3gpp2 & U32(MBN_DATA_CLASS_1XRTT)) != 0 &&
              (netcountry::kDataClass3gpp2 & U32(MBN_DATA_CLASS_1XEVDO)) != 0 &&
              (netcountry::kDataClass3gpp2 & U32(MBN_DATA_CLASS_UMB)) != 0);

// The connected default route with the lowest metric in one family's forward
// table (netcountry::ElectDefaultRoute), 0 for none. The same table and the
// same two interface reads as NetworkConfig::DiscoverEgress.
uint32_t ElectedDefaultInterface(ADDRESS_FAMILY family) {
  PMIB_IPFORWARD_TABLE2 table = nullptr;
  if (::GetIpForwardTable2(family, &table) != NO_ERROR || !table) return 0;
  std::vector<netcountry::DefaultRoute> routes;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_IPFORWARD_ROW2& row = table->Table[i];
    if (row.DestinationPrefix.PrefixLength != 0) continue;
    MIB_IPINTERFACE_ROW ifRow{};
    ifRow.Family = family;
    ifRow.InterfaceLuid = row.InterfaceLuid;
    ULONG ifMetric = 0;
    if (::GetIpInterfaceEntry(&ifRow) == NO_ERROR) ifMetric = ifRow.Metric;
    MIB_IF_ROW2 ifEntry{};
    ifEntry.InterfaceLuid = row.InterfaceLuid;
    bool connected = true;
    if (::GetIfEntry2(&ifEntry) == NO_ERROR)
      connected = ifEntry.MediaConnectState != MediaConnectStateDisconnected;
    routes.push_back(netcountry::DefaultRoute{
        .ifIndex = row.InterfaceIndex,
        .metric = static_cast<uint64_t>(row.Metric) + ifMetric,
        .connected = connected,
    });
  }
  ::FreeMibTable(table);
  return netcountry::ElectDefaultRoute(routes);
}

// Registry-style GUID text, which is how MbnApi names an adapter
// (IMbnInterface::get_InterfaceID).
std::string GuidText(const GUID& g) {
  return std::format(
      "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
      g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
      g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// A BSTR as UTF-8, "" for none.
std::string BstrText(BSTR s) {
  return s ? Narrow(std::wstring_view(s, ::SysStringLen(s))) : std::string();
}

// The adapter MbnApi knows by the interface GUID `ifGuid`, or none.
Microsoft::WRL::ComPtr<IMbnInterface> MobileBroadbandAdapter(const GUID& ifGuid) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IMbnInterfaceManager> manager;
  if (FAILED(::CoCreateInstance(__uuidof(MbnInterfaceManager), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&manager))))
    return nullptr;
  SAFEARRAY* adapters = nullptr;
  if (FAILED(manager->GetInterfaces(&adapters)) || !adapters) return nullptr;
  const std::string wanted = GuidText(ifGuid);
  ComPtr<IMbnInterface> found;
  VARTYPE type = VT_EMPTY;
  LONG lower = 0;
  LONG upper = -1;
  // An array of interface pointers, which SafeArrayGetElement AddRefs into the
  // ComPtr it fills; anything else is not what GetInterfaces documents.
  if (SUCCEEDED(::SafeArrayGetVartype(adapters, &type)) &&
      (type == VT_UNKNOWN || type == VT_DISPATCH) &&
      SUCCEEDED(::SafeArrayGetLBound(adapters, 1, &lower)) &&
      SUCCEEDED(::SafeArrayGetUBound(adapters, 1, &upper))) {
    for (LONG i = lower; i <= upper && !found; ++i) {
      ComPtr<IUnknown> element;
      if (FAILED(::SafeArrayGetElement(adapters, &i, element.GetAddressOf())) || !element)
        continue;
      ComPtr<IMbnInterface> adapter;
      if (FAILED(element.As(&adapter))) continue;
      BSTR id = nullptr;
      if (FAILED(adapter->get_InterfaceID(&id))) continue;
      const bool same = netcountry::SameInterfaceGuid(BstrText(id), wanted);
      ::SysFreeString(id);
      if (same) found = adapter;
    }
  }
  ::SafeArrayDestroy(adapters);
  return found;
}

// The registration of the adapter `ifGuid`, into `facts`. Only the register
// state, the current data class and the provider id are read: never the
// interface capabilities or the subscriber information, which carry the
// device's and the SIM's identifiers.
void ReadRegistrationInApartment(const GUID& ifGuid, netcountry::MobileBroadbandFacts& facts) {
  using Microsoft::WRL::ComPtr;
  const ComPtr<IMbnInterface> adapter = MobileBroadbandAdapter(ifGuid);
  if (!adapter) return;
  ComPtr<IMbnRegistration> registration;
  if (FAILED(adapter.As(&registration))) return;
  MBN_REGISTER_STATE state = MBN_REGISTER_STATE_NONE;
  if (FAILED(registration->GetRegisterState(&state))) return;
  facts.registerState = static_cast<uint32_t>(state);
  // Not registered: there is no provider id to read, and that is the answer.
  if (!netcountry::IsRegistered(facts.registerState)) {
    facts.readable = true;
    return;
  }
  ULONG dataClass = 0;
  if (SUCCEEDED(registration->GetCurrentDataClass(&dataClass))) facts.dataClass = dataClass;
  BSTR providerId = nullptr;
  if (FAILED(registration->GetProviderID(&providerId))) return;
  facts.providerId = BstrText(providerId);
  ::SysFreeString(providerId);
  facts.readable = true;
}

// COM for the length of the read, on whichever thread asks
// (NetworkCountryWatch's). S_OK and S_FALSE are balanced below;
// RPC_E_CHANGED_MODE means the thread is already an STA, which serves as it is.
// Every interface pointer is released inside ReadRegistrationInApartment,
// before the apartment is left.
void ReadRegistration(const GUID& ifGuid, netcountry::MobileBroadbandFacts& facts) {
  const HRESULT init = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(init) && init != RPC_E_CHANGED_MODE) return;
  ReadRegistrationInApartment(ifGuid, facts);
  if (SUCCEEDED(init)) ::CoUninitialize();
}

// NotifyRouteChange2's callback, on a system thread.
void WINAPI OnRouteChange(PVOID context, PMIB_IPFORWARD_ROW2 row, MIB_NOTIFICATION_TYPE) {
  // Default routes only: the tun's capture routes are installed and reverted
  // in bursts that say nothing about which network carries the default route.
  if (row && row->DestinationPrefix.PrefixLength != 0) return;
  static_cast<const DefaultRouteChanges*>(context)->Notify();
}

// NotifyIpInterfaceChange's callback, on a system thread: every interface
// change can move the default route.
void WINAPI OnInterfaceChange(PVOID context, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
  static_cast<const DefaultRouteChanges*>(context)->Notify();
}

}  // namespace

// See the contract in the header: the default route first, MbnApi only for a
// mobile broadband adapter.
netcountry::Reading ReadNetworkCountry() {
  netcountry::MobileBroadbandFacts facts;
  // The IPv4 default route carries this PC's traffic wherever there is one; an
  // IPv6-only mobile network has only the other.
  uint32_t index = ElectedDefaultInterface(AF_INET);
  if (index == 0) index = ElectedDefaultInterface(AF_INET6);
  facts.defaultRoute = index != 0;
  if (facts.defaultRoute) {
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = index;
    if (::GetIfEntry2(&row) == NO_ERROR) {
      facts.mobileBroadband = netcountry::IsMobileBroadbandInterface(
          row.Type, static_cast<uint32_t>(row.PhysicalMediumType));
      if (facts.mobileBroadband) ReadRegistration(row.InterfaceGuid, facts);
    }
  }
  return netcountry::ReadingFor(facts);
}

DefaultRouteChanges::DefaultRouteChanges(std::function<void()> sink) : sink_(std::move(sink)) {
  HANDLE route = nullptr;
  if (const DWORD err = ::NotifyRouteChange2(AF_UNSPEC, &OnRouteChange, this, FALSE, &route);
      err == NO_ERROR) {
    routeHandle_ = route;
  } else {
    LogWarn("network country: NotifyRouteChange2 failed ({}); a default route that "
            "moves will not be read until an interface changes",
            err);
  }
  HANDLE interfaces = nullptr;
  if (const DWORD err =
          ::NotifyIpInterfaceChange(AF_UNSPEC, &OnInterfaceChange, this, FALSE, &interfaces);
      err == NO_ERROR) {
    interfaceHandle_ = interfaces;
  } else {
    LogWarn("network country: NotifyIpInterfaceChange failed ({}); an interface "
            "change will not be read until a default route moves",
            err);
  }
}

DefaultRouteChanges::~DefaultRouteChanges() {
  if (routeHandle_) ::CancelMibChangeNotify2(static_cast<HANDLE>(routeHandle_));
  if (interfaceHandle_) ::CancelMibChangeNotify2(static_cast<HANDLE>(interfaceHandle_));
}

}  // namespace urnw
