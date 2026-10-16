// Executable spec for what the tray says about the connection (App/
// TrayConnectionState.h): Connected, in words and in the icon, only with the
// tunnel up and the health connected; with the tunnel up the health says how
// far the connection got; and with it down the machine's own state comes first.
// Run against the header the app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App -I ../src/Common tray-connection-state-tests.cpp -o /tmp/tray-tests && /tmp/tray-tests
//
// tests/tray_connection_state_test.go runs it, with negative controls that
// rewrite a copy of the header, and pins AppController's and TrayIcon's use of
// it.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "TrayConnectionState.h"

namespace tray = urnw::tray;
using Health = urnw::health::State;
using tray::Action;
using tray::Status;
using tray::Tunnel;

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

const char* Name(Status status) {
  switch (status) {
    case Status::None: return "none";
    case Status::Connected: return "connected";
    case Status::FindingProviders: return "finding providers";
    case Status::Degraded: return "degraded";
    case Status::Connecting: return "connecting";
    case Status::Failed: return "failed";
    case Status::Disconnecting: return "disconnecting";
    case Status::BlockedKillSwitch: return "blocked by the kill switch";
    case Status::BlockedNoTunnel: return "blocked with no tunnel";
    case Status::FailsafeStopped: return "failsafe stopped";
  }
  return "?";
}

const char* Name(Tunnel tunnel) {
  switch (tunnel) {
    case Tunnel::Down: return "down";
    case Tunnel::Starting: return "starting";
    case Tunnel::Up: return "up";
    case Tunnel::Stopping: return "stopping";
    case Tunnel::Failed: return "failed";
  }
  return "?";
}

std::string Name(std::optional<Health> health) {
  return health ? urnw::health::ToString(*health) : "no health";
}

// Signed in, with a status: the tunnel and the health as given, nothing else
// installed.
tray::Facts Known(Tunnel tunnel, std::optional<Health> health = std::nullopt) {
  tray::Facts facts;
  facts.signedIn = true;
  facts.statusKnown = true;
  facts.tunnel = tunnel;
  facts.health = health;
  return facts;
}

const std::vector<std::optional<Health>> kEveryHealth = {
    std::nullopt,       Health::NoService, Health::Disconnected, Health::Connecting,
    Health::Evaluating, Health::Connected, Health::Degraded,     Health::Failed,
};

const std::vector<std::string_view> kEveryWfpState = {"off", "", "armed", "connecting",
                                                      "connected"};

// Connected, and the connected icon, only with the tunnel up and the health
// connected, or the tunnel's own claim with no stats since it came up.
void TestConnectedNeedsTheTunnelUp() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Starting, Tunnel::Up, Tunnel::Stopping,
                        Tunnel::Failed}) {
    for (const auto& health : kEveryHealth) {
      for (std::string_view wfp : kEveryWfpState) {
        for (bool routes : {false, true}) {
          tray::Facts facts = Known(tunnel, health);
          facts.wfpState = wfp;
          facts.routesInstalled = routes;
          const tray::View view = tray::Decide(facts);
          const bool connected = tunnel == Tunnel::Up &&
                                 (!health || *health == Health::Connected);
          const std::string what = std::string("connected only with the tunnel up: tunnel ") +
                                   Name(tunnel) + ", " + Name(health) + ", wfp \"" +
                                   std::string(wfp) + "\"" + (routes ? ", routes" : "");
          Check((view.status == Status::Connected) == connected, what + " reads " +
                                                                    Name(view.status));
          Check(view.connectedIcon == connected, what + (view.connectedIcon
                                                             ? " shows the connected icon"
                                                             : " hides the connected icon"));
        }
      }
    }
  }
}

// With the tunnel up the health says how far the connection got, in the
// connect page's words.
void TestTheTunnelUpReadsItsHealth() {
  const struct {
    std::optional<Health> health;
    Status status;
  } cases[] = {
      {std::nullopt, Status::Connected},           {Health::Connected, Status::Connected},
      {Health::Evaluating, Status::FindingProviders}, {Health::Degraded, Status::Degraded},
      {Health::Connecting, Status::Connecting},    {Health::Failed, Status::Failed},
      {Health::Disconnected, Status::None},        {Health::NoService, Status::None},
  };
  for (const auto& c : cases) {
    const Status status = tray::Decide(Known(Tunnel::Up, c.health)).status;
    Check(status == c.status, std::string("the tunnel up reads its health: ") +
                                  Name(c.health) + " reads " + Name(status) + ", not " +
                                  Name(c.status));
  }
}

// With the tunnel down, the machine's own state first (#41), in its order:
// routes, the armed kill switch, another policy, the failsafe stop.
void TestTheTunnelDownLinesKeepTheirOrder() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Stopping}) {
    const std::string where = std::string("the tunnel ") + Name(tunnel) + ": ";
    Check(tray::Decide(Known(tunnel)).status == Status::None,
          where + "nothing installed reads as the app name alone");
    Check(tray::Decide(Known(tunnel, Health::Connected)).status == Status::None,
          where + "a stale connected health is not a connection");

    tray::Facts routes = Known(tunnel);
    routes.routesInstalled = true;
    routes.wfpState = "armed";
    routes.failsafeStop = true;
    Check(tray::Decide(routes).status == Status::Disconnecting,
          where + "routes still installed come first");

    tray::Facts armed = Known(tunnel);
    armed.wfpState = "armed";
    armed.failsafeStop = true;
    Check(tray::Decide(armed).status == Status::BlockedKillSwitch,
          where + "the armed kill switch comes before the failsafe stop");

    for (std::string_view wfp : {"connecting", "connected"}) {
      tray::Facts policy = Known(tunnel);
      policy.wfpState = wfp;
      policy.failsafeStop = true;
      Check(tray::Decide(policy).status == Status::BlockedNoTunnel,
            where + "a \"" + std::string(wfp) + "\" policy with no tunnel reads as blocked");
    }

    tray::Facts failsafe = Known(tunnel);
    failsafe.failsafeStop = true;
    Check(tray::Decide(failsafe).status == Status::FailsafeStopped,
          where + "the failsafe stop says the tunnel failed");
  }
}

// Nothing known yet: the app name alone, and no connected icon.
void TestNoStatusClaimsNothing() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Starting, Tunnel::Up, Tunnel::Stopping,
                        Tunnel::Failed}) {
    for (const auto& health : kEveryHealth) {
      tray::Facts facts = Known(tunnel, health);
      facts.statusKnown = false;
      facts.routesInstalled = true;
      facts.wfpState = "armed";
      const tray::View view = tray::Decide(facts);
      Check(view.status == Status::None && !view.connectedIcon,
            std::string("no status claims nothing: tunnel ") + Name(tunnel) + ", " +
                Name(health) + " reads " + Name(view.status));
    }
  }
}

// proto::TunnelState's enumerators, by name (tests/tray_connection_state_test.go
// holds this list to Protocol.h's).
enum class StandInTunnelState { Stopped, Starting, Up, Stopping, Error, RpcOnly, Preparing };

void TestTheServiceStatesFold() {
  const struct {
    StandInTunnelState state;
    Tunnel tunnel;
    const char* name;
  } cases[] = {
      {StandInTunnelState::Stopped, Tunnel::Down, "stopped"},
      {StandInTunnelState::Starting, Tunnel::Starting, "starting"},
      {StandInTunnelState::Up, Tunnel::Up, "up"},
      {StandInTunnelState::Stopping, Tunnel::Stopping, "stopping"},
      {StandInTunnelState::Error, Tunnel::Failed, "error"},
      {StandInTunnelState::RpcOnly, Tunnel::Down, "rpc only"},
      {StandInTunnelState::Preparing, Tunnel::Starting, "preparing"},
  };
  for (const auto& c : cases) {
    const Tunnel tunnel = tray::TunnelOf(c.state);
    Check(tunnel == c.tunnel, std::string("the service state ") + c.name + " folds to " +
                                  Name(tunnel) + ", not " + Name(c.tunnel));
  }
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*run)();
  } tests[] = {
      {"connected needs the tunnel up", TestConnectedNeedsTheTunnelUp},
      {"the tunnel up reads its health", TestTheTunnelUpReadsItsHealth},
      {"the tunnel-down lines keep their order", TestTheTunnelDownLinesKeepTheirOrder},
      {"no status claims nothing", TestNoStatusClaimsNothing},
      {"the service states fold", TestTheServiceStatesFold},
  };
  for (const auto& test : tests) {
    const int before = gFailures;
    test.run();
    std::cout << (gFailures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }
  std::cout << gCases << " checks, " << gFailures << " failed\n";
  return gFailures == 0 ? 0 : 1;
}
