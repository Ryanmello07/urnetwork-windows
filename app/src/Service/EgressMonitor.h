// Watches for network changes and keeps the SDK's egress interface binding (R1)
// pointed at the current physical default-route interface, so the service's own
// platform/provider sockets never loop into the tunnel it provides. Mirrors what
// wireguard-windows does with NotifyIpInterfaceChange.
//
// The binding must be in force BEFORE the SDK opens any socket and BEFORE the
// tun routes are installed — see the ordering in TunnelController::StartLocked.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Same order as NetworkConfig.cpp (which compiles): winsock2 before the IP
// helpers, and ws2tcpip pulls in ws2ipdef (SOCKADDR_INET) that netioapi needs.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>    // NET_LUID, MIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE, NotifyIpInterfaceChange
#include <wlanapi.h>

#include <functional>
#include <mutex>

#include "NetworkChangeNotify.h"  // EgressChangeFilter
#include "NetworkConfig.h"

namespace urnw {

// Windows reports Wi-Fi signal quality as 0..100. Five stable buckets match
// the user-visible bars and keep one-point RSSI jitter out of the SDK.
inline constexpr int WifiSignalLevel(uint32_t quality) {
  return static_cast<int>(((quality > 100 ? 100 : quality) * 5) / 101);
}

// The first OS value establishes the current radio state. Later notifications
// only matter when they cross a user-visible bar boundary.
class WifiSignalLevelTracker {
 public:
  bool Observe(uint32_t quality) {
    const int next = WifiSignalLevel(quality);
    if (level_ < 0) {
      level_ = next;
      return false;
    }
    if (level_ == next) return false;
    level_ = next;
    return true;
  }

  void Reset() { level_ = -1; }

 private:
  int level_ = -1;
};

class EgressMonitor {
 public:
  // What the monitor does with what it observes.
  //
  //   Bind        — the tunnel session's (and rpc-only's) monitor: everything
  //                 this header describes, R1 binding included.
  //   ObserveOnly — the provider-only device's network watch
  //                 (TunnelController::WatchProviderNetworkLocked). There is no
  //                 tun, and that device deliberately binds nothing: its
  //                 sockets follow the route table like any other process's
  //                 (Common/ProvideLifecycle.h). It therefore never binds an
  //                 interface and never calls setEgressInterfaceIndex
  //                 (process-global inside this service's SDK). It reads the
  //                 route table's election only to tell a network that moved
  //                 from churn that did not (EgressChangeFilter), and reports,
  //                 through SetOnNetworkEvent and SetOnNetworkQualityEvent, what
  //                 the binding monitor would report.
  //
  // Neither reports anything from Start(): starting to watch is not a network
  // change, and the reading Start() takes is the baseline every later one is
  // compared with.
  enum class Binding { Bind, ObserveOnly };

  // tunLuid is the interface to EXCLUDE from egress selection. A zero LUID
  // means "there is no tun" — the rpc-only start mode, which creates no
  // adapter. That is not a sentinel bolted on: DiscoverEgress excludes by
  // `row.InterfaceLuid.Value == tunLuid.Value`, and no real interface has LUID
  // 0 (NET_LUID packs a non-zero IANA ifType into its high bits), so a zero
  // LUID excludes exactly nothing, which is exactly right when nothing needs
  // excluding. No LUID is faked and no invariant is papered over.
  //
  // With no tun, R1 does not apply — the SDK's sockets cannot loop into a
  // tunnel that does not exist — so the binding is a plain "prefer the physical
  // default route", and the retention behaviour in Refresh() is conservative
  // rather than load-bearing.
  explicit EgressMonitor(NET_LUID tunLuid, Binding binding = Binding::Bind)
      : tunLuid_(tunLuid), binding_(binding) {}
  ~EgressMonitor();

  EgressMonitor(const EgressMonitor&) = delete;
  EgressMonitor& operator=(const EgressMonitor&) = delete;

  // Notified after the binding MOVES — the bound interface, or the exact address
  // a socket on it leaves from (EgressChange::binding) — with the bound indices.
  // Everything else pinned to the physical interface, the split-tunnel driver's
  // source address above all, has to follow what the SDK just followed, or it
  // keeps rewriting binds to an adapter that is gone or an address it no longer
  // holds: another network on the same adapter keeps the index and changes the
  // address.
  //
  // Runs on the thread that observed the change: a system worker thread for a
  // notification, the caller's thread for the initial Start(), which reports the
  // binding it starts from. Invoked WITHOUT this monitor's lock. The handler
  // must not take a lock that a thread calling Stop() could be holding — Stop()
  // blocks until in-flight callbacks return, so such a handler deadlocks against
  // it. Set before Start().
  using ChangeHandler = std::function<void(EgressInterfaces)>;
  void SetOnChange(ChangeHandler handler);

  // "THE NETWORK THE SDK'S SOCKETS RIDE MOVED." Fired when an observation moves
  // it (EgressChange::network): the bound interface, whether it can carry a
  // socket, the network it is on or its gateway. That is a different question
  // from the one above, and the difference is the reason it is a second
  // callback.
  //
  // SetOnChange alone is blind to the single most common failure this service
  // has: the cable comes out, the adapter keeps its default route for a while,
  // DiscoverEgress deliberately falls back to it (NetworkConfig.cpp) and Refresh
  // deliberately retains the last good index anyway — so nothing pinned to the
  // interface moves, and the SDK, never told, would let its transports die on
  // timeouts with the tunnel "up" over nothing. What did move is that the
  // interface can no longer carry a socket, and that is a network event here.
  //
  // What moves nothing the SDK's sockets ride is NOT one: a Hyper-V or WSL
  // adapter coming up, the stale default route of a NIC already left behind, a
  // lease renewed with the same address. Every OS observation used to be, and
  // each kicked every transport in the process, cancelling the dial a real
  // switch had just started.
  //
  // Same threading contract as SetOnChange — a system worker thread, no lock of
  // this monitor held — with one ADDITIONAL rule that is not advice: THIS
  // HANDLER MUST NOT CALL INTO THE SDK. Stop() blocks until in-flight callbacks
  // return, so a handler that can block wedges Stop(), which wedges
  // TearDownSessionLocked. Record the event and return; do the work on a thread
  // of your own (TunnelWatchdog.h). Never fired from Start().
  using NetworkEventHandler = std::function<void()>;
  void SetOnNetworkEvent(NetworkEventHandler handler);

  // Wi-Fi signal-bar changes do not invalidate working sockets. They request
  // transfer estimator remeasurement without entering the reconnect path.
  using NetworkQualityEventHandler = std::function<void()>;
  void SetOnNetworkQualityEvent(NetworkQualityEventHandler handler);

  // Compute the current egress interfaces, push them to the SDK, and register
  // for change notifications to keep them current. Returns false if the
  // notification could not be registered; the initial binding is still applied,
  // it just will not follow later network changes. The reading it starts from
  // is the baseline later observations are compared with, never a network
  // event.
  bool Start();
  void Stop();

  // An observation: recompute, bind, and report what moved (the change
  // callbacks call it).
  void Refresh();

  // What is currently bound, for status and for the startup log. index4 == 0
  // means the SDK's sockets are NOT pinned to a physical interface — with the
  // tunnel up that is the R1 loop condition.
  EgressInterfaces Current() const;

 private:
  static void __stdcall OnChange(void* context, MIB_IPINTERFACE_ROW* row,
                                 MIB_NOTIFICATION_TYPE type);
  // The DEFAULT ROUTE half of "the network moved". A default route appearing or
  // vanishing is a ROUTE event, and this monitor watched only interface events —
  // so the cable coming out, the case that strands the owner, could produce no
  // observation at all. DiscoverEgress reads the forward table, so the two
  // notifications answer the same question and both must feed Refresh.
  //
  // FILTERED, and the filter is load-bearing rather than an optimisation: the
  // tun carries 31 capture routes (NetPolicy.h) that are installed and reverted
  // in a burst at every bring-up and teardown, and an unfiltered subscription
  // would turn each of those bursts into 31 notifications and 31 SDK
  // network-change kicks — during the exact sequences that must not be
  // perturbed. Only prefix-length-0 routes on interfaces other than our tun get
  // through.
  static void __stdcall OnRouteChange(void* context, MIB_IPFORWARD_ROW2* row,
                                      MIB_NOTIFICATION_TYPE type);
  // The ADDRESS half. A lease that hands the same adapter another address, and
  // an address finishing duplicate detection after the route that made its
  // interface the egress (a source still tentative is not usable), raise no
  // interface or route event, and both move what EgressChangeFilter compares.
  // Unfiltered but for our tun: what moves nothing is no change, so address
  // churn on other adapters costs a read and no more.
  static void __stdcall OnAddressChange(void* context, MIB_UNICASTIPADDRESS_ROW* row,
                                        MIB_NOTIFICATION_TYPE type);
  static void WINAPI OnWlanChange(PWLAN_NOTIFICATION_DATA data, void* context);

  // The two kinds of reading Update takes.
  //   Baseline    — Start()'s: binds, and seeds the change filter. Reports no
  //                 network event, and the binding it starts from to
  //                 SetOnChange only.
  //   Observation — an OS notification's (Refresh): binds, and reports what
  //                 moved.
  enum class Reading { Baseline, Observation };
  void Update(Reading reading);

  NET_LUID tunLuid_;
  const Binding binding_;
  HANDLE notifyHandle_ = nullptr;
  HANDLE routeNotifyHandle_ = nullptr;
  HANDLE addressNotifyHandle_ = nullptr;
  HANDLE wlanHandle_ = nullptr;

  // Serializes Update: NotifyIpInterfaceChange callbacks arrive on system
  // worker threads and can overlap each other and Start().
  mutable std::mutex mutex_;
  EgressInterfaces current_;
  // What the last reading saw a socket of each family would ride.
  EgressChangeFilter filter_;
  ChangeHandler onChange_;
  NetworkEventHandler onNetworkEvent_;
  NetworkQualityEventHandler onNetworkQualityEvent_;
  WifiSignalLevelTracker wlanSignalLevelTracker_;
};

}  // namespace urnw
