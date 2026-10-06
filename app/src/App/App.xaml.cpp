// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "App.xaml.h"

#include <functional>
#include <string>

#include "AppController.h"
#include "Log.h"
#include "SingleInstance.h"
#include "Startup.h"
#include "Strings.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

// cppwinrt's generated module.g.cpp (the activation-factory dispatcher) links against the
// global ::winrt_make_URnetwork_App(). Unlike MainWindow (Microsoft.UI.Xaml.Window), cppwinrt
// emits NO factory_implementation::App / composition ctor for App (a Microsoft.UI.Xaml.
// Application subclass) - App.g.cpp references them but App.g.h / URnetwork.2.h never declare
// them, so App.g.cpp cannot be compiled. That is correct: App is a singleton created via
// Application::Start -> winrt::make<implementation::App> (main.cpp) and is NEVER activated by
// name (the winrt::URnetwork::App projection is never constructed), so this maker is never
// invoked. Returning null is the honest answer - App has no activation factory - and lets
// URnetwork.exe link. Global namespace to match module.g.cpp's extern declaration.
void* winrt_make_URnetwork_App() { return nullptr; }

namespace winrt::URnetwork::implementation {

App::App() {
  // An exception that reaches XAML ends the process. In a tray app nothing
  // visibly closes when that happens — the icon simply stops being there — so
  // this handler is the last chance to name the cause. Registered in every
  // configuration, not only _DEBUG: Release is the build the owner runs.
  // Handled() is deliberately NOT set: masking it would leave a half-dead app.
  UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
    const std::wstring message{e.Message()};
    urnw::LogError("app: unhandled exception: {}", urnw::Narrow(message));
#if defined(_DEBUG)
    if (IsDebuggerPresent()) __debugbreak();
#endif
    // the process ends after this box, so a launch meanwhile starts afresh
    urnw::BeginExiting();
    urnw::FailVisible(L"URnetwork hit an unexpected error and has to close.", message);
  });
  urnw::LogInfo("app: XAML Application constructed");
}

// A tray app: OnLaunched brings up the controller (tray + SDK) but does not open
// a window — the user opens it from the tray. Every step is logged, and the two
// that can fail before there is any UI end in a message box: from outside, "the
// SDK threw" and "the app is fine, look in the notification area" are the same
// picture.
void App::OnLaunched(LaunchActivatedEventArgs const&) {
  // main.cpp already opened the log (its first instruction) — this is the entry
  // marker for the XAML side of the handoff, and the proof for wWinMain that
  // XAML got this far at all.
  urnw::MarkLaunched();
  urnw::LogInfo("app: OnLaunched");

  // A launch that fails here exits as every ending does: it begins exiting
  // first (SingleInstance.h), before the message box holds this thread, so a
  // launch that reaches it meanwhile is refused and starts the app once this
  // process has ended, instead of waiting on a window that will never exist.
  try {
    urnw::SdkInit(/*isService=*/false, 64ll * 1024 * 1024);
  } catch (const std::exception& e) {
    urnw::BeginExiting();
    urnw::FailVisible(
        L"The URnetwork SDK could not be initialized, so the app cannot run.\n\n"
        L"Check that URnetworkSdk.dll sits next to URnetwork.exe.",
        urnw::Widen(e.what()));
    if (auto app = Application::Current()) app.Exit();
    return;
  } catch (...) {
    urnw::BeginExiting();
    urnw::FailVisible(
        L"The URnetwork SDK could not be initialized, so the app cannot run.",
        L"An unknown exception escaped SdkInit.");
    if (auto app = Application::Current()) app.Exit();
    return;
  }

  try {
    auto controller = std::make_unique<urnw::AppController>();
    controller->Start();
    urnw::SetApp(std::move(controller));
  } catch (winrt::hresult_error const& e) {
    urnw::BeginExiting();
    urnw::FailVisible(L"URnetwork could not start its tray icon and SDK host.",
                      std::wstring{e.message()});
    if (auto app = Application::Current()) app.Exit();
    return;
  } catch (const std::exception& e) {
    urnw::BeginExiting();
    urnw::FailVisible(L"URnetwork could not start its tray icon and SDK host.",
                      urnw::Widen(e.what()));
    if (auto app = Application::Current()) app.Exit();
    return;
  }
  urnw::LogInfo("app: controller started");

  // Here, and not earlier: the tray icon has already resolved a localized
  // string, so the resource loader is cached either way and this only reads what
  // the UI itself got. Run before that and this probe would BE the first lookup,
  // and a probe failing for its own reasons would leave every string in the UI
  // rendering as a key id for the rest of the process (see Startup.h).
  urnw::LogInfo("startup: {}", urnw::Narrow(urnw::ResourceProbe()));

  // urnetwork:// protocol activation — the ur.io/wallet-connect bridge returning
  // through the browser (see main.cpp). First the activation this instance was
  // launched with, if any (the app wasn't running when the wallet returned). We
  // are on the UI thread here.
  namespace lifecycle = winrt::Microsoft::Windows::AppLifecycle;
  try {
    auto instance = lifecycle::AppInstance::GetCurrent();
    std::string url = urnw::DeepLinkFromActivation(instance.GetActivatedEventArgs());
    if (url.empty()) url = urnw::LaunchDeepLink();  // fallback: our own command line
    if (!url.empty()) urnw::App().HandleDeepLink(url);
  } catch (winrt::hresult_error const& e) {
    // Not fatal: the app runs, only the wallet callback would be missed. Loud in
    // the log rather than on screen.
    urnw::LogError("app: activation routing not wired: {}",
                   urnw::Narrow(std::wstring{e.message()}));
  }

  // Then the launches redirected here while the app is running. main.cpp
  // subscribes AppInstance::Activated before it registers the key, and the gate
  // held whatever arrived while this instance started; opening it serves those
  // now and posts each later one to this thread (Activated fires on a
  // background thread), until the instance begins to exit.
  try {
    auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    urnw::Activations().Open(
        [queue](std::function<void()> work) {
          return queue.TryEnqueue([work = std::move(work)] { work(); });
        },
        [](const std::string& url) {
          if (url.empty()) {
            // A plain relaunch — the user ran URnetwork.exe again, usually
            // because the first launch "did nothing" (it went to the
            // notification area). Show the window: an app that ignores being
            // launched is the same silent non-event all over again.
            urnw::LogInfo("app: relaunched while running — showing the window");
            urnw::App().ShowWindow(nullptr);
            return;
          }
          urnw::App().HandleDeepLink(url);
        });
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("app: relaunches not wired: {}", urnw::Narrow(std::wstring{e.message()}));
  } catch (const std::exception& e) {
    urnw::LogError("app: relaunches not wired: {}", e.what());
  }
  urnw::LogInfo("app: launch complete (tray icon is the only UI until it is clicked)");
}

}  // namespace winrt::URnetwork::implementation
