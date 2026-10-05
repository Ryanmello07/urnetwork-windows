// How the tray app ends, and what each ending leaves running in the service
// (owner decision, 2026-10-05; support inbox 1521).
//
// THE DECISION. "Quit on Windows should stop the background provider, as
// Linux does. Closing the window should minimize to the system tray. Quit in
// the system tray menu should quit the provider and background work."
//
// WHAT IT CHANGES. The service owns the tunnel session and, while there is no
// session, the provider-only device (ProvideLifecycle.h). The tray's Quit used
// to leave both running there: it tore down the window, the tray and the app's
// own timers and exited, and the next launch adopted whatever the service still
// ran. A user who quit stayed connected and kept providing with no app on
// screen, and after an unexpected drop with the kill switch on the armed floor
// kept the machine blocked with nothing left to lift it.
//
// THE ENDINGS, and what each asks of the service:
//   Quit              the tray menu's Quit, the user's explicit one. stop_tunnel
//                     ends any session, tunnel or rpc-only, and lifts whatever
//                     firewall policy is in force, the armed floor included,
//                     exactly as Disconnect lifts it (StopLocked with
//                     finalDisarm); stop_provider retires the provider-only
//                     device. Before either, the app stops everything of its own
//                     that could start them again (SdkHost::Quit), so the
//                     service ends with no session and no provider, and nothing
//                     starts them until the app runs again. The next launch
//                     starts fresh: the resume finds no session to reattach to
//                     (D8) and the provider reconcile starts what the stored
//                     provide mode says.
//   CloseRequest      a WM_CLOSE sent to the tray's window from outside the app:
//                     `taskkill /im URnetwork.exe` without /f, or an installer.
//   InstallerHandoff  the in-app updater started the MSI, which needs the app's
//                     files. The MSI stops the service itself (ServiceControl
//                     Stop="both"), and the session and the provider go with it.
// The last two exit the app and leave the service as it is, which is what they
// always did: neither is the user asking to stop anything.
//
// NOT ENDINGS, so not in the table:
//   * Closing the main window (its X, Alt+F4, the taskbar's or the system
//     menu's Close) hides it to the tray, and the tunnel and the provider carry
//     on (AppController's AppWindow.Closing handler). Alt+F4 on the tray's own
//     hidden window, which holds the foreground after its menu closes, is the
//     same gesture aimed at a window nobody can see, and does nothing
//     (TrayIcon::WndProc).
//   * Signing out of URnetwork. SdkHost::Logout stops the tunnel and the
//     service's logout retires the provider, and the app stays in the tray.
//   * Windows shutting down or the user signing out of Windows. The session
//     end takes the app down without any of the above. At shutdown the SCM
//     stops the service, which ends the session and the provider; at a sign-out
//     the service keeps running them, and the next launch adopts them.
//
// Pure and header-only, constexpr, no Windows headers and no allocation, like
// ConnectAction.h and ProvideLifecycle.h: tools/app-lifetime-tests.cpp runs it
// on any host, and AppController::Shutdown asks PlanFor what an ending stops.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::lifetime {

enum class Ending { Quit, CloseRequest, InstallerHandoff };

// For logs.
constexpr const char* ToString(Ending ending) {
  switch (ending) {
    case Ending::Quit: return "quit";
    case Ending::CloseRequest: return "close request";
    case Ending::InstallerHandoff: return "installer handoff";
  }
  return "unknown";
}

// Every ending exits the app: its own timers, the update checker, the balance
// store, the tray and the window stop, then Application::Exit. What differs is
// the service.
struct Plan {
  // stop_tunnel, then stop_provider, after the app has closed every way of
  // starting either again (SdkHost::Quit). False leaves the service as it is.
  bool stopService = false;
  // For the log. Never empty.
  const char* why = "";
};

constexpr Plan PlanFor(Ending ending) {
  Plan p;
  switch (ending) {
    case Ending::Quit:
      p.stopService = true;
      p.why =
          "the user quit: the tunnel and the provider stop with the app, and "
          "nothing starts them until it runs again";
      return p;
    case Ending::CloseRequest:
      p.why =
          "asked to close from outside the app: the service keeps what it "
          "runs, and the next launch adopts it";
      return p;
    case Ending::InstallerHandoff:
      p.why =
          "the installer is replacing the app: it stops the service itself";
      return p;
  }
  p.why = "an ending this build does not know: the service is left as it is";
  return p;
}

}  // namespace urnw::lifetime
