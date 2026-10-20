// SPDX-License-Identifier: MPL-2.0
#include "EgressMonitor.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>

#include <algorithm>

#include "Log.h"
#include "NetworkConfig.h"
#include "Sdk.h"
#include "Strings.h"
#include "ThreadGuard.h"

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "wlanapi.lib")

namespace urnw {
namespace {

// The v4 source address of the chosen egress interface, for the log. This is
// the value the R1 audit hangs on: it must be the physical adapter's LAN
// address, never the tun's 169.254.x.
std::string SourceAddressOf(uint32_t ifIndex) {
  if (ifIndex == 0) return "-";
  uint8_t addr[4] = {0};
  if (!NetworkConfig::InterfaceSourceAddress(ifIndex, AF_INET, addr)) return "-";
  return std::format("{}.{}.{}.{}", addr[0], addr[1], addr[2], addr[3]);
}

// Does this interface index still name something? Retaining an index that has
// been reclaimed is worse than dropping it: setsockopt fails and the socket
// falls back to the route table, which with the tunnel up is the tun.
bool InterfaceExists(uint32_t ifIndex) {
  if (ifIndex == 0) return false;
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = ifIndex;
  return ::GetIfEntry2(&row) == NO_ERROR;
}

}  // namespace

EgressMonitor::~EgressMonitor() { Stop(); }

bool EgressMonitor::Start() {
  // The reading watching starts from: bound (by a binding monitor), and the
  // baseline every later reading is compared with. Never a network event, in
  // either mode: starting to watch is not the network moving, and the tunnel
  // session's capture readiness used to keep this one and replay it as a kick
  // into the session device's first platform dial (see Binding).
  Update(Reading::Baseline);
  bool ok = true;
  DWORD err = ::NotifyIpInterfaceChange(AF_UNSPEC, &EgressMonitor::OnChange,
                                        this, FALSE, &notifyHandle_);
  if (err != NO_ERROR) {
    // The initial binding stands, but it will not follow a Wi-Fi/ethernet
    // switch — say so, because that failure mode looks like "the tunnel stopped
    // working after I moved networks".
    LogError("egress: NotifyIpInterfaceChange failed: {} — the binding will NOT "
             "follow network changes",
             err);
    ok = false;
  }
  // THE ROUTE HALF. Registered separately and tolerated separately: either
  // subscription alone still keeps the binding roughly current, and the pair is
  // what makes "the default route went away" observable at all. AF_UNSPEC so a
  // v6-only default vanishing is seen too; the callback filters.
  err = ::NotifyRouteChange2(AF_UNSPEC, &EgressMonitor::OnRouteChange, this,
                             FALSE, &routeNotifyHandle_);
  if (err != NO_ERROR) {
    LogError("egress: NotifyRouteChange2 failed: {} — a DEFAULT ROUTE appearing "
             "or vanishing will not be observed. That is the cable-out case, so "
             "the sdk will not be told the network moved and the dead-tunnel "
             "failsafe becomes the only thing that notices.",
             err);
    ok = false;
  }
  // THE ADDRESS HALF (see OnAddressChange). Tolerated like the route half, and
  // less load-bearing: without it an address that changes with no interface or
  // route event is seen at the next one of those instead of at once.
  err = ::NotifyUnicastIpAddressChange(AF_UNSPEC, &EgressMonitor::OnAddressChange, this,
                                       FALSE, &addressNotifyHandle_);
  if (err != NO_ERROR) {
    LogWarn("egress: NotifyUnicastIpAddressChange failed: {} — a new lease on the "
            "same adapter, or an address finishing duplicate detection, is seen "
            "only at the next interface or route event",
            err);
  }
  DWORD negotiatedVersion = 0;
  err = ::WlanOpenHandle(2, nullptr, &negotiatedVersion, &wlanHandle_);
  if (err == ERROR_SUCCESS) {
    err = ::WlanRegisterNotification(
        wlanHandle_, WLAN_NOTIFICATION_SOURCE_MSM, FALSE,
        &EgressMonitor::OnWlanChange, this, nullptr, nullptr);
  }
  if (err != ERROR_SUCCESS) {
    LogWarn("egress: Wi-Fi quality notifications unavailable ({}); adaptive "
            "pacing will continue from transport feedback", err);
    if (wlanHandle_) {
      ::WlanCloseHandle(wlanHandle_, nullptr);
      wlanHandle_ = nullptr;
    }
  }
  return ok;
}

void EgressMonitor::Stop() {
  // Cancel first and WITHOUT the lock: CancelMibChangeNotify2 blocks until any
  // in-flight callback has returned, and that callback takes mutex_.
  if (notifyHandle_) {
    ::CancelMibChangeNotify2(notifyHandle_);
    notifyHandle_ = nullptr;
  }
  if (routeNotifyHandle_) {
    ::CancelMibChangeNotify2(routeNotifyHandle_);
    routeNotifyHandle_ = nullptr;
  }
  if (addressNotifyHandle_) {
    ::CancelMibChangeNotify2(addressNotifyHandle_);
    addressNotifyHandle_ = nullptr;
  }
  if (wlanHandle_) {
    ::WlanRegisterNotification(wlanHandle_, WLAN_NOTIFICATION_SOURCE_NONE,
                               FALSE, nullptr, nullptr, nullptr, nullptr);
    ::WlanCloseHandle(wlanHandle_, nullptr);
    wlanHandle_ = nullptr;
  }
  {
    std::scoped_lock lock(mutex_);
    wlanSignalLevelTracker_.Reset();
  }
}

void EgressMonitor::SetOnChange(ChangeHandler handler) {
  std::scoped_lock lock(mutex_);
  onChange_ = std::move(handler);
}

void EgressMonitor::SetOnNetworkEvent(NetworkEventHandler handler) {
  std::scoped_lock lock(mutex_);
  onNetworkEvent_ = std::move(handler);
}

void EgressMonitor::SetOnNetworkQualityEvent(NetworkQualityEventHandler handler) {
  std::scoped_lock lock(mutex_);
  onNetworkQualityEvent_ = std::move(handler);
}

void EgressMonitor::Refresh() { Update(Reading::Observation); }

void EgressMonitor::Update(Reading reading) {
  // Observe only: report a network that moved and stop. No binding and no R1
  // log line — none of it applies to a device that binds nothing. The route
  // table's election is read, never bound, only to tell a move from churn.
  if (binding_ == Binding::ObserveOnly) {
    NetworkEventHandler observed;
    bool moved = false;
    {
      std::scoped_lock lock(mutex_);
      const EgressSignature signature =
          NetworkConfig::ObserveEgress(NetworkConfig::DiscoverEgress(tunLuid_));
      if (reading == Reading::Baseline) {
        filter_.Seed(signature);
      } else {
        moved = filter_.Observe(signature).network;
      }
      observed = onNetworkEvent_;
    }
    if (moved && observed) observed();
    return;
  }

  ChangeHandler handler;
  NetworkEventHandler networkEvent;
  EgressInterfaces egress;
  EgressChange change;

  // Zero LUID == the rpc-only mode's "no tun exists" (see the ctor). Every R1
  // claim below is about looping into the tunnel, so with no tunnel they are
  // not merely quieter, they are untrue — and a log that cries R1 when R1 does
  // not apply is how a real one gets ignored later.
  const bool hasTun = tunLuid_.Value != 0;

  {
    std::scoped_lock lock(mutex_);
    egress = NetworkConfig::DiscoverEgress(tunLuid_);

    // Losing the physical default route (Wi-Fi drops, cable out) makes
    // DiscoverEgress return 0. Do NOT push 0 down while we have a good index:
    // unbinding means the SDK's next socket follows the route table, which with
    // the tunnel up is the tun — the exact self-deadlock R1 exists to prevent.
    // A socket pinned to a downed interface fails fast with WSAENETUNREACH and
    // retries; a socket that follows the tun blackholes silently. Keep the last
    // known good and wait for the next change notification.
    //
    // But only while that index still names a real interface. Pinning to an
    // index the stack has reclaimed is the worst of both worlds: setsockopt
    // fails, and the socket falls back to the route table anyway — the loop,
    // arrived at quietly. Re-validating is what makes the retention safe rather
    // than merely preferable.
    if (egress.index4 == 0 && current_.index4 != 0) {
      if (InterfaceExists(current_.index4)) {
        LogWarn("egress: no ipv4 default route right now — keeping the last "
                "physical interface {} bound rather than unbinding ({})",
                current_.index4, hasTun ? "R1" : "no tun, so this is only a "
                                                 "preference, not R1");
        egress.index4 = current_.index4;
      } else if (hasTun) {
        LogError("egress: the retained ipv4 interface {} no longer exists, so it "
                 "cannot stay pinned. The sdk's sockets are unbound and will "
                 "follow the route table — R1 exposure while the tunnel is up, "
                 "until an interface reappears.",
                 current_.index4);
      } else {
        LogWarn("egress: the retained ipv4 interface {} no longer exists; "
                "unbinding. With no tun, the sdk's sockets following the route "
                "table is the correct behaviour, not an exposure.",
                current_.index4);
      }
    }
    if (egress.index6 == 0 && current_.index6 != 0 &&
        InterfaceExists(current_.index6)) {
      egress.index6 = current_.index6;
    }

    const bool indexMoved =
        egress.index4 != current_.index4 || egress.index6 != current_.index6;
    current_ = egress;
    // BEFORE anything is reported, so the kick the report leads to re-dials over
    // this binding and not the one it replaces.
    urnet::setEgressInterfaceIndex(static_cast<int64_t>(egress.index4),
                                   static_cast<int64_t>(egress.index6));

    // What a socket of each family rides now, against the last reading. The
    // baseline only seeds the comparison, and reports to SetOnChange the
    // binding it starts from.
    const EgressSignature signature = NetworkConfig::ObserveEgress(egress);
    if (reading == Reading::Baseline) {
      filter_.Seed(signature);
      change.binding = indexMoved;
    } else {
      change = filter_.Observe(signature);
      change.binding = change.binding || indexMoved;
    }

    if (indexMoved) {
      LogInfo("egress: bound to v4=[{}] src={} v6=[{}]",
              NetworkConfig::DescribeInterface(egress.index4),
              SourceAddressOf(egress.index4),
              NetworkConfig::DescribeInterface(egress.index6));
    } else if (change.network || change.binding) {
      LogInfo("egress: still bound to v4={} v6={}, and what a socket on it rides "
              "moved (link {}, src={}) — {}",
              egress.index4, egress.index6, signature.v4.usable ? "usable" : "UNUSABLE",
              SourceAddressOf(egress.index4),
              change.network ? "telling the sdk the network moved"
                             : "only the address followers move");
    } else {
      LogDebug("egress: unchanged (v4={} v6={})", egress.index4, egress.index6);
    }

    if (egress.index4 == 0) {
      // Nothing to pin to at all. Harmless before the tun routes exist; once
      // they do it is the R1 loop condition, so it is an error in tunnel mode.
      if (hasTun) {
        LogError("egress: NO physical ipv4 interface bound — the sdk's own "
                 "sockets will follow the route table (R1 exposure once tun "
                 "routes are up)");
      } else {
        LogWarn("egress: no physical ipv4 default route — the sdk's own sockets "
                "will follow the route table. With no tun that is normal; it "
                "usually just means this machine has no network right now.");
      }
    }
    handler = onChange_;
    networkEvent = onNetworkEvent_;
  }

  // Deliberately outside the lock: the handler takes locks of its own, and
  // Stop() blocks on this callback while its caller holds one of them.
  if (change.binding && handler) handler(egress);
  // On what moved the network, not on what moved the binding: the dominant
  // failure (the cable out under a retained index) moves no binding, and is a
  // network change here because the interface became unusable. See the note on
  // SetOnNetworkEvent.
  if (change.network && networkEvent) networkEvent();
}

EgressInterfaces EgressMonitor::Current() const {
  std::scoped_lock lock(mutex_);
  return current_;
}

void __stdcall EgressMonitor::OnChange(void* context, MIB_IPINTERFACE_ROW* row,
                                       MIB_NOTIFICATION_TYPE) {
  // Called on a system worker thread. Recompute the egress binding; the SDK
  // setter is atomic and cheap, so we can react to every change.
  //
  // GUARDED, because "a system worker thread" means a thread that ran none of
  // our startup and therefore had no terminate handler of ours (ThreadGuard.h).
  // Refresh() calls into the SDK, allocates and formats, so it can throw, and an
  // escape here used to be an unattributable abort. Arming the handler on a
  // pooled OS thread reaches beyond our own code, and that is acceptable in one
  // direction only: our handler adds a log line and a route revert to a process
  // that was already dying, and never keeps one alive.
  auto* self = static_cast<EgressMonitor*>(context);
  // Configuring our prepared adapter is not an uplink change. Feeding it back
  // into readiness would invalidate every capture transaction's own proof.
  if (self && row && self->tunLuid_.Value != 0 &&
      row->InterfaceLuid.Value == self->tunLuid_.Value) return;
  RunGuarded("egress-change", [&] {
    if (self) self->Refresh();
  });
}

void __stdcall EgressMonitor::OnRouteChange(void* context,
                                            MIB_IPFORWARD_ROW2* row,
                                            MIB_NOTIFICATION_TYPE type) {
  auto* self = static_cast<EgressMonitor*>(context);
  if (!self) return;
  // MibInitialNotification arrives with a NULL row and says nothing about any
  // route; the baseline Start() reads has already covered that ground.
  if (type == MibInitialNotification || row == nullptr) return;
  // ONLY DEFAULT ROUTES, and never our own tun's. See the note on this function
  // in the header: without the filter, every bring-up and every teardown storms
  // this callback 31 times over.
  if (row->DestinationPrefix.PrefixLength != 0) return;
  if (self->tunLuid_.Value != 0 &&
      row->InterfaceLuid.Value == self->tunLuid_.Value)
    return;
  RunGuarded("egress-route-change", [&] { self->Refresh(); });
}

void __stdcall EgressMonitor::OnAddressChange(void* context,
                                              MIB_UNICASTIPADDRESS_ROW* row,
                                              MIB_NOTIFICATION_TYPE type) {
  auto* self = static_cast<EgressMonitor*>(context);
  if (!self) return;
  // As for routes: the initial notification describes no address.
  if (type == MibInitialNotification || row == nullptr) return;
  // Our tun's addresses are capture configuration, not the uplink: the same
  // reason OnChange skips it.
  if (self->tunLuid_.Value != 0 &&
      row->InterfaceLuid.Value == self->tunLuid_.Value)
    return;
  RunGuarded("egress-address-change", [&] { self->Refresh(); });
}

void WINAPI EgressMonitor::OnWlanChange(PWLAN_NOTIFICATION_DATA data,
                                        void* context) {
  auto* self = static_cast<EgressMonitor*>(context);
  if (!self) return;
  RunGuarded("egress-wlan-quality", [&] {
    if (!data || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_MSM ||
        data->NotificationCode != wlan_notification_msm_signal_quality_change ||
        data->dwDataSize < sizeof(ULONG) || data->pData == nullptr)
      return;
    const ULONG quality = *static_cast<const ULONG*>(data->pData);
    NetworkQualityEventHandler handler;
    {
      std::scoped_lock lock(self->mutex_);
      if (!self->wlanSignalLevelTracker_.Observe(quality)) return;
      handler = self->onNetworkQualityEvent_;
    }
    if (handler) handler();
  });
}

}  // namespace urnw
