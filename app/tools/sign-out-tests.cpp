// Executable spec for signing out of URnetwork on Windows (Common/SignOut.h,
// owner decision 2026-10-05): a sign-out sends Quit's stop_tunnel and
// stop_provider, in Quit's order, then the service's logout; one the service
// could not be told stays owed in a marker that outlives the app, is delivered
// before anything else once the service can be reached, and holds every start
// until then; and the next sign-in starts what its settings say and nothing
// else, as a fresh launch does. Run against the same headers the app compiles,
// on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common sign-out-tests.cpp -o /tmp/sign-out-tests && /tmp/sign-out-tests
//
// The service is a fake with TunnelController's semantics where a sign-out
// meets them: stop_tunnel ends the session and retires the provider-only
// device; stop_provider retires the device; logout does both and deletes the
// identity and the credential the sdk stored; a start makes the identity once
// and stores its credential; a restart ends what ran, keeps what is on disk and
// starts nothing by itself (the service's main.cpp). A wedged session lock
// lets stop_tunnel answer through its escape and refuses the other two
// (StopBudget.h). The app is modeled as SdkHost runs it, and those lines are
// pinned in SdkHost by tests/sign_out_wiring_test.go: every pass delivers an
// owed sign-out first, a signed-out pass provides nothing, and nothing starts
// while a sign-out is owed. Nothing here waits on a clock.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "ProvideLifecycle.h"
#include "SignOut.h"

namespace provide = urnw::provide;
namespace signout = urnw::signout;
using signout::Delivery;
using signout::Request;

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

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const std::string& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  return "[" + out + "]";
}

const std::vector<std::string> kSignOutRequests{"stop_tunnel", "stop_provider", "logout"};

// The service, as far as a sign-out meets it.
class FakeService {
 public:
  // The control channel can be dialled: false while the service is not running
  // or its pipe cannot be reached.
  bool reachable = true;
  // A connect wedged inside the sdk holds the session lock.
  bool wedged = false;
  // stop_tunnel goes unanswered (the pipe broke under it).
  bool dropStopTunnel = false;
  // Every request that reached the service, in order.
  std::vector<std::string> log;
  // Called before each sign-out request is served.
  std::function<void(Request)> onRequest;

  // What runs and for whom, which ends with the process.
  std::string tunnelAccount;
  std::string providerAccount;
  int tunnelIdentity = 0;
  int providerIdentity = 0;
  // What is on disk, which a restart keeps: the device identity (0 for none)
  // and the client credential the sdk stored for the space.
  int identity = 0;
  std::string storedCredential;

  bool Serve(Request request) {
    if (onRequest) onRequest(request);
    log.push_back(signout::ToString(request));
    switch (request) {
      case Request::StopTunnel:
        if (dropStopTunnel) return false;
        // the lock-free escape gives the routes back and ends nothing else
        if (!wedged) StopAll();
        return true;
      case Request::StopProvider:
        if (wedged) return false;
        RetireProvider();
        return true;
      case Request::Logout:
        if (wedged) return false;
        StopAll();
        identity = 0;
        storedCredential.clear();
        return true;
    }
    return false;
  }

  // start_provider: refused while a session runs, whose device provides.
  void StartProvider(const std::string& account) {
    log.push_back("start_provider " + account);
    if (!tunnelAccount.empty()) return;
    providerAccount = account;
    providerIdentity = IdentityFor(account);
  }

  // start_tunnel: every bring-up opens with a teardown, the provider-only
  // device included.
  void StartTunnel(const std::string& account) {
    log.push_back("start_tunnel " + account);
    RetireProvider();
    tunnelAccount = account;
    tunnelIdentity = IdentityFor(account);
  }

  // The process ends, by a crash, an update or a reboot, and starts again.
  void Restart() {
    tunnelAccount.clear();
    tunnelIdentity = 0;
    RetireProvider();
  }

  // What one get_state says to the app's reconcile (proto::ProviderFactsFrom).
  provide::ServiceProviderFacts Facts() const {
    provide::ServiceProviderFacts facts;
    facts.answered = true;
    facts.tunnelSession = !tunnelAccount.empty();
    facts.killSwitchArmed = false;
    facts.providerRunning = !providerAccount.empty();
    return facts;
  }

  bool Runs() const { return !tunnelAccount.empty() || !providerAccount.empty(); }

  std::vector<std::string> Since(std::size_t at) const {
    return std::vector<std::string>(log.begin() + static_cast<std::ptrdiff_t>(at), log.end());
  }

 private:
  int nextIdentity_ = 1;

  // A device persists a new identity only when none is stored
  // (NewDeviceLocked), and stores the credential it starts with.
  int IdentityFor(const std::string& account) {
    if (identity == 0) identity = nextIdentity_++;
    storedCredential = account;
    return identity;
  }
  void StopAll() {
    tunnelAccount.clear();
    tunnelIdentity = 0;
    RetireProvider();
  }
  void RetireProvider() {
    providerAccount.clear();
    providerIdentity = 0;
  }
};

// The app, reduced to what a sign-out meets, as SdkHost runs it.
class App {
 public:
  // `markerFile` is the marker on disk, which outlives the app. `account` is
  // the credential stored when the app starts ("" signed out), and
  // `provideMode` the stored provide control mode.
  App(FakeService& service, bool& markerFile, std::string account, std::string provideMode)
      : service_(service),
        obligation_(signout::Marker{
            .read = [&markerFile] { return markerFile; },
            .write = [&markerFile](bool owed) { markerFile = owed; },
        }),
        account_(std::move(account)),
        provideMode_(std::move(provideMode)) {
    // Initialize
    obligation_.Load();
  }

  // Initialize's first pass.
  void Launch() { Pass(); }

  // RegisterNetworkClient: the credential is stored, then a provider pass.
  void SignIn(std::string account, std::string provideMode) {
    account_ = std::move(account);
    provideMode_ = std::move(provideMode);
    Pass();
  }

  // Logout: signed out, the local credentials and the settings stored with
  // them logged out, then the obligation begun.
  Delivery SignOut() {
    account_.clear();
    provideMode_ = "never";
    return obligation_.Begin(Bind());
  }

  // A Connect press: a pass, whose bootstrap starts a session only while
  // signed in and with nothing owed.
  void Connect() {
    obligation_.Settle(Bind());
    if (account_.empty() || !service_.reachable || obligation_.Owed()) return;
    service_.StartTunnel(account_);
  }

  // A session-worker pass with no session: the watchdog's, a launch's, a
  // sign-in's. An owed sign-out first, then the provider reconcile.
  void Pass() {
    obligation_.Settle(Bind());
    if (!service_.reachable) return;
    const std::string mode = account_.empty() ? "never" : provideMode_;
    switch (provide::DisconnectedProviderStep(mode, service_.Facts())) {
      case provide::DisconnectedStep::None:
        return;
      case provide::DisconnectedStep::Stop:
        service_.Serve(Request::StopProvider);
        return;
      case provide::DisconnectedStep::Start:
        if (obligation_.Owed()) return;
        service_.StartProvider(account_);
        return;
    }
  }

  bool Owed() const { return obligation_.Owed(); }

 private:
  signout::Service Bind() {
    return signout::Service{
        .reach = [this] { return service_.reachable; },
        .send = [this](Request request) { return service_.Serve(request); },
    };
  }

  FakeService& service_;
  signout::Obligation obligation_;
  std::string account_;
  std::string provideMode_;
};

bool AnyStartFor(const std::vector<std::string>& log, const std::string& account) {
  for (const std::string& line : log) {
    if (line == "start_provider " + account || line == "start_tunnel " + account) return true;
  }
  return false;
}

void TestSignOutStopsAsQuitThenLogsOut() {
  // signed in with a session, and signed in providing with none
  for (const bool connected : {true, false}) {
    const std::string where = connected ? "sign-out, connected: " : "sign-out, providing: ";
    FakeService service;
    bool marker = false;
    App app(service, marker, "", "never");
    app.SignIn("a", "always");
    if (connected) app.Connect();
    Check(service.Runs() && service.identity != 0, where + "(the fake) a runs on an identity");
    const std::size_t at = service.log.size();
    const Delivery delivery = app.SignOut();
    const std::vector<std::string> sent = service.Since(at);
    Check(delivery == Delivery::Delivered, where + "delivered to a service that can be reached");
    Check(sent.size() >= 2 && sent[0] == "stop_tunnel" && sent[1] == "stop_provider",
          where + "Quit's requests in Quit's order, stop_tunnel then stop_provider (got " +
              Join(sent) + ")");
    Check(sent == kSignOutRequests,
          where + "then the logout, and nothing else (got " + Join(sent) + ")");
    Check(!service.Runs(), where + "nothing runs in the service");
    Check(service.identity == 0 && service.storedCredential.empty(),
          where + "the service keeps neither the identity nor a's credential");
    Check(!marker && !app.Owed(), where + "nothing is owed once the service has done it");
  }
}

void TestSignOutMissedServiceIsToldWhenItComesBack() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "always");
  service.reachable = false;
  const Delivery delivery = app.SignOut();
  Check(delivery == Delivery::Unreachable, "unreachable: nothing could be sent");
  Check(marker && app.Owed(), "unreachable: the sign-out is owed, in the marker");
  Check(service.providerAccount == "a", "unreachable: (the fake) the service still provides for a");
  // the watchdog's pass once the pipe is back
  service.reachable = true;
  const std::size_t at = service.log.size();
  app.Pass();
  const std::vector<std::string> sent = service.Since(at);
  Check(sent == kSignOutRequests,
        "comes back: the first requests deliver the sign-out, and nothing starts (got " +
            Join(sent) + ")");
  Check(!service.Runs(), "comes back: a's provider is stopped");
  Check(service.identity == 0 && service.storedCredential.empty(),
        "comes back: a's identity and credential are gone");
  Check(!marker && !app.Owed(), "comes back: nothing is owed");
}

void TestSignOutMissedServiceRestartDoesNotResume() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "network");
  app.Connect();
  const std::size_t signOut = service.log.size();
  service.reachable = false;
  app.SignOut();
  // the service restarts: it runs nothing, and its disk still holds a
  service.Restart();
  Check(!service.Runs(), "restart: the service starts nothing by itself");
  Check(service.identity != 0 && service.storedCredential == "a",
        "restart: (the fake) a's identity and credential are still on disk");
  // the app relaunches, signed out, before the service can be reached
  App relaunched(service, marker, "", "never");
  Check(relaunched.Owed(), "relaunch: the owed sign-out survives the app");
  relaunched.Launch();
  service.reachable = true;
  const std::size_t at = service.log.size();
  relaunched.Pass();
  const std::vector<std::string> sent = service.Since(at);
  Check(sent == kSignOutRequests,
        "relaunch: the first pass delivers the owed sign-out (got " + Join(sent) + ")");
  Check(service.identity == 0 && service.storedCredential.empty(),
        "relaunch: a's identity and credential are gone");
  Check(!service.Runs() && !AnyStartFor(service.Since(signOut), "a"),
        "restart and relaunch: nothing of a's runs or starts after the sign-out");
  Check(!marker, "relaunch: nothing is owed once delivered");
}

void TestSignOutOwedHoldsTheNextSignIn() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "always");
  const int identityOfA = service.identity;
  service.reachable = false;
  app.SignOut();
  // a reboot: the service comes up after the app, which relaunches signed out
  service.Restart();
  App rebooted(service, marker, "", "never");
  rebooted.Launch();
  // b signs in while the service still cannot be reached
  rebooted.SignIn("b", "always");
  Check(rebooted.Owed() && !AnyStartFor(service.log, "b"),
        "reboot: b starts nothing while a's sign-out is owed");
  service.reachable = true;
  const std::size_t at = service.log.size();
  rebooted.Pass();
  const std::vector<std::string> sent = service.Since(at);
  const std::vector<std::string> want{"stop_tunnel", "stop_provider", "logout", "start_provider b"};
  Check(sent == want, "reboot: a's sign-out is delivered before b's provider starts (got " +
                          Join(sent) + ")");
  Check(service.providerAccount == "b" && service.providerIdentity != 0 &&
            service.providerIdentity != identityOfA,
        "reboot: b provides on a new identity, never on a's");
  Check(service.storedCredential == "b", "reboot: the service holds b's credential alone");
}

void TestSignOutRefusedStaysOwed() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "always");
  const int identityOfA = service.identity;
  service.wedged = true;
  const std::size_t at = service.log.size();
  const Delivery delivery = app.SignOut();
  Check(delivery == Delivery::Refused, "wedged: the service refused the sign-out");
  Check(service.Since(at) == kSignOutRequests,
        "wedged: all three requests go out although stop_provider was refused (got " +
            Join(service.Since(at)) + ")");
  Check(marker && app.Owed(), "wedged: the sign-out stays owed");
  // b signs in while the lock is still wedged
  app.SignIn("b", "always");
  Check(app.Owed() && !AnyStartFor(service.log, "b"),
        "wedged: b does not start beside a's provider, nor on a's identity");
  // a restart is what ends a wedge
  service.wedged = false;
  service.Restart();
  const std::size_t after = service.log.size();
  app.Pass();
  const std::vector<std::string> sent = service.Since(after);
  const std::vector<std::string> want{"stop_tunnel", "stop_provider", "logout", "start_provider b"};
  Check(sent == want, "wedge ended: the sign-out first, then b (got " + Join(sent) + ")");
  Check(service.providerIdentity != identityOfA && !marker,
        "wedge ended: b on a new identity, and nothing owed");
}

void TestSignOutNextSignInStartsAsAFreshLaunch() {
  for (const char* mode : {"never", "always", "network", "auto"}) {
    // a signs out, b signs in
    FakeService service;
    bool marker = false;
    App app(service, marker, "", "never");
    app.SignIn("a", "always");
    app.Connect();
    app.SignOut();
    const std::size_t at = service.log.size();
    app.SignIn("b", mode);
    const std::vector<std::string> afterSignOut = service.Since(at);
    // b on a machine that never saw a
    FakeService fresh;
    bool freshMarker = false;
    App launched(fresh, freshMarker, "b", mode);
    launched.Launch();
    const std::string where = std::string("sign-in after a sign-out, mode ") + mode + ": ";
    Check(afterSignOut == fresh.log, where + "starts what a fresh launch starts (got " +
                                         Join(afterSignOut) + ", fresh " + Join(fresh.log) + ")");
    const bool provides = provide::ProviderRuns(provide::ControlModeFrom(mode), /*connected=*/false);
    Check(AnyStartFor(afterSignOut, "b") == provides,
          where + (provides ? "the provider starts" : "nothing starts"));
    bool tunnel = false;
    for (const std::string& line : afterSignOut) tunnel = tunnel || line.rfind("start_tunnel", 0) == 0;
    Check(!tunnel, where + "no tunnel without a Connect (D8)");
    app.Connect();
    Check(service.tunnelAccount == "b", where + "a Connect starts b's tunnel");
  }
}

void TestSignOutMarkerIsWrittenBeforeAnyRequest() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "always");
  bool markedAtFirst = false;
  bool first = true;
  service.onRequest = [&](Request) {
    if (first) markedAtFirst = marker;
    first = false;
  };
  app.SignOut();
  Check(!first && markedAtFirst,
        "marker: written before the first request, so an app ended mid-delivery still owes it");
}

void TestSignOutSendsEveryRequestWhenOneFails() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "", "never");
  app.SignIn("a", "always");
  service.dropStopTunnel = true;
  const std::size_t at = service.log.size();
  const Delivery delivery = app.SignOut();
  Check(service.Since(at) == kSignOutRequests,
        "one fails: stop_provider and logout still go out (got " + Join(service.Since(at)) + ")");
  Check(delivery == Delivery::Refused && marker, "one fails: the sign-out stays owed");
  service.dropStopTunnel = false;
  app.Pass();
  Check(!marker && !app.Owed(), "one fails: the next pass delivers it");
}

void TestNothingOwedSendsNothing() {
  FakeService service;
  bool marker = false;
  App app(service, marker, "a", "always");
  app.Launch();
  const std::size_t at = service.log.size();
  app.Pass();
  app.Connect();
  for (const std::string& line : service.Since(at)) {
    Check(line != "logout" && line != "stop_tunnel",
          "nothing owed: a pass sends no sign-out request (got " + line + ")");
  }
  Check(service.identity != 0 && service.storedCredential == "a",
        "nothing owed: a signed-in account keeps its identity and credential");
}

void TestNames() {
  Check(std::string(signout::ToString(Request::StopTunnel)) == "stop_tunnel", "names: stop_tunnel");
  Check(std::string(signout::ToString(Request::StopProvider)) == "stop_provider",
        "names: stop_provider");
  Check(std::string(signout::ToString(Request::Logout)) == "logout", "names: logout");
  for (const Delivery delivery : {Delivery::Delivered, Delivery::Unreachable, Delivery::Refused}) {
    Check(std::string(signout::ToString(delivery)) != "unknown", "names: every delivery");
  }
}

}  // namespace

int main() {
  TestSignOutStopsAsQuitThenLogsOut();
  TestSignOutMissedServiceIsToldWhenItComesBack();
  TestSignOutMissedServiceRestartDoesNotResume();
  TestSignOutOwedHoldsTheNextSignIn();
  TestSignOutRefusedStaysOwed();
  TestSignOutNextSignInStartsAsAFreshLaunch();
  TestSignOutMarkerIsWrittenBeforeAnyRequest();
  TestSignOutSendsEveryRequestWhenOneFails();
  TestNothingOwedSendsNothing();
  TestNames();
  std::cout << (gCases - gFailures) << "/" << gCases << " sign-out checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
