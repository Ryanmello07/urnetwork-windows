// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The call sites of the hand-over between a launch and the one running
// instance (Common/InstanceHandover.h): a launch during the moment Quit spends
// stopping the service used to be redirected to the quitting instance and
// lost, or to be told after 15 s that URnetwork was already running but did not
// respond. What the gate and the launch decide runs in instance_handover_test.go;
// wWinMain, App::OnLaunched and AppController need Windows and WinRT, so these
// read their sources with every comment blanked.

// main.cpp, comments stripped.
func appMainSource(t *testing.T) string {
	return stripComments(readAppSource(t, "main.cpp"))
}

// Each catch branch of text that ends the process with exit begins exiting
// before its message box, and there are want of them.
func relaunchRequireExitingBeforeEachBox(t *testing.T, where, text, exit string, want int) {
	t.Helper()
	exits := 0
	for _, branch := range strings.Split(text, "catch (")[1:] {
		if !strings.Contains(branch, exit) {
			continue
		}
		exits++
		begin := strings.Index(branch, "urnw::BeginExiting();")
		box := strings.Index(branch, "urnw::FailVisible(")
		if begin < 0 || box < 0 || begin > box {
			t.Errorf("a failure exit in %s does not begin exiting before its message box:\n%s", where, branch)
		}
	}
	if exits != want {
		t.Errorf("%s has %d failure exits, want %d; update this contract", where, exits, want)
	}
}

// Every launch is taken through the gate. Each process creates its exiting
// signal and subscribes AppInstance::Activated before it registers the key (the
// App SDK consumes a redirect that arrives with no handler); the handler hands
// each launch to the gate and holds the redirect for the gate's decision; and
// App::OnLaunched opens the gate once the controller exists, serving held
// launches and posting later ones to the UI thread, each as it asks
// (AppController::ServeLaunch, launch_window_wiring_test.go).
func TestRelaunchDuringQuitEveryLaunchGoesThroughTheGate(t *testing.T) {
	main := appMainSource(t)
	entry := definitionBody(t, "main.cpp", main, "int __stdcall wWinMain(")
	signOutRequireInOrder(t, "wWinMain", entry,
		regexp.QuoteMeta("urnw::CreateExitingSignal();"),
		regexp.QuoteMeta("AppInstance current = AppInstance::GetCurrent();"),
		regexp.QuoteMeta("args = current.GetActivatedEventArgs();"),
		regexp.QuoteMeta("current.Activated("),
		regexp.QuoteMeta("TakeRedirectedLaunch(redirected);"),
		regexp.QuoteMeta("primary = AppInstance::FindOrRegisterForKey(kInstanceKey);"))
	// before the first registration, not merely before some later retry of it
	signal := strings.Index(entry, "urnw::CreateExitingSignal();")
	subscribe := strings.Index(entry, "current.Activated(")
	register := strings.Index(entry, "AppInstance::FindOrRegisterForKey(")
	if signal < 0 || subscribe < 0 || register < 0 || signal > register || subscribe > register {
		t.Error("wWinMain must create the exiting signal and subscribe Activated before its first " +
			"FindOrRegisterForKey: a redirect that arrives with no handler is consumed and lost")
	}
	take := definitionBody(t, "main.cpp", main, "void TakeRedirectedLaunch(AppActivationArguments const& redirected) {")
	signOutRequireInOrder(t, "TakeRedirectedLaunch", take,
		regexp.QuoteMeta("urnw::LaunchRequestFromActivation(redirected);"),
		regexp.QuoteMeta("urnw::Activations().Take("),
		regexp.QuoteMeta("urnw::instance::kServeBudget"))

	// one subscription, before the key: OnLaunched no longer subscribes
	app := stripComments(readAppSource(t, "App.xaml.cpp"))
	if strings.Count(main, ".Activated(") != 1 || strings.Contains(app, ".Activated(") {
		t.Error("AppInstance::Activated must be subscribed once, in wWinMain before the key is registered")
	}
	launched := definitionBody(t, "App.xaml.cpp", app, "void App::OnLaunched(LaunchActivatedEventArgs const&) {")
	signOutRequireInOrder(t, "App::OnLaunched", launched,
		regexp.QuoteMeta("urnw::SetApp(std::move(controller));"),
		regexp.QuoteMeta("urnw::Activations().Open("),
		regexp.QuoteMeta("queue.TryEnqueue("),
		regexp.QuoteMeta("urnw::App().ServeLaunch(request);"))
	if strings.Count(launched, "urnw::Activations().Open(") != 1 {
		t.Error("App::OnLaunched must open the gate exactly once")
	}
}

// Every ending refuses launches before anything holds the UI thread. Shutdown
// begins exiting right after its once-guard, before the update checker's join
// and long before SdkHost::Quit; a launch that fails before its window can
// exist, XAML failing to start and an unhandled exception each begin exiting
// before their message box; and beginning to exit raises the exiting signal
// before the gate refuses anything.
func TestRelaunchDuringQuitEveryEndingRefusesLaunchesFirst(t *testing.T) {
	controller := appControllerSource(t)
	shutdown := definitionBody(t, "AppController.cpp", controller,
		"void AppController::Shutdown(lifetime::Ending ending) {")
	signOutRequireInOrder(t, "AppController::Shutdown", shutdown,
		regexp.QuoteMeta("if (quitting_.exchange(true, std::memory_order_acq_rel)) return;"),
		regexp.QuoteMeta("BeginExiting();"),
		regexp.QuoteMeta("updates_.Stop();"),
		regexp.QuoteMeta("tray_.Destroy();"),
		regexp.QuoteMeta("if (plan.stopService) sdk_.Quit();"),
		regexp.QuoteMeta("app.Exit();"))
	if strings.Count(controller, "BeginExiting();") != 1 {
		t.Error("AppController.cpp must begin exiting in Shutdown alone, which every ending reaches")
	}

	// each of OnLaunched's failure exits begins exiting before its box, and so
	// does each of wWinMain's own exits once XAML has been started
	app := stripComments(readAppSource(t, "App.xaml.cpp"))
	launched := definitionBody(t, "App.xaml.cpp", app, "void App::OnLaunched(LaunchActivatedEventArgs const&) {")
	relaunchRequireExitingBeforeEachBox(t, "App::OnLaunched", launched, "app.Exit();", 4)
	unhandled := handlerSource(t, "App.xaml.cpp", app, "UnhandledException(")
	signOutRequireInOrder(t, "App's UnhandledException handler", unhandled,
		regexp.QuoteMeta("urnw::BeginExiting();"),
		regexp.QuoteMeta("urnw::FailVisible(L\"URnetwork hit an unexpected error and has to close.\""))
	entry := definitionBody(t, "main.cpp", appMainSource(t), "int __stdcall wWinMain(")
	at := strings.Index(entry, "Application::Start(")
	if at < 0 {
		t.Fatal("wWinMain no longer calls Application::Start; update this contract")
	}
	started := entry[at:]
	relaunchRequireExitingBeforeEachBox(t, "wWinMain after Application::Start", started, "return 1;", 3)
	signOutRequireInOrder(t, "wWinMain after the message loop", started,
		regexp.QuoteMeta("} catch (...) {"),
		regexp.QuoteMeta("return 1;"),
		regexp.QuoteMeta("}"),
		regexp.QuoteMeta("urnw::BeginExiting();"),
		regexp.QuoteMeta("if (!urnw::WasLaunched()) {"))

	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	provideRequire(t, "urnw::BeginExiting", definitionBody(t, "SingleInstance.cpp", glue, "void BeginExiting() {"),
		"Activations().Close();")
	gate := definitionBody(t, "SingleInstance.cpp", glue, "instance::ActivationGate& Activations() {")
	signOutRequireInOrder(t, "urnw::Activations", gate,
		regexp.QuoteMeta("new instance::ActivationGate("),
		regexp.QuoteMeta("if (g_exitingSignal) ::SetEvent(g_exitingSignal);"))
	create := definitionBody(t, "SingleInstance.cpp", glue, "void CreateExitingSignal() {")
	signOutRequireInOrder(t, "urnw::CreateExitingSignal", create,
		regexp.QuoteMeta("ExitingSignalName(::GetCurrentProcessId());"),
		`::CreateEventW\(nullptr,\s*TRUE,\s*FALSE,`,
		regexp.QuoteMeta("if (::GetLastError() == ERROR_ALREADY_EXISTS) ::ResetEvent(g_exitingSignal);"))
	provideRequire(t, "SingleInstance.cpp's ExitingSignalName", glue, "ids::kExitingSignalPrefix, processId")
	provideRequire(t, "Ids.h", stripComments(readCommonSource(t, "Ids.h")),
		`inline constexpr wchar_t kExitingSignalPrefix[] = L"Local\\URnetwork.Desktop.Exiting.";`)
}

// Every launch goes through instance::Launch before the app starts (the first
// launch holds the key at once): a hand-over exits, every failure shows its
// box and ends the process, and only the holder goes on to
// Application::Start. The redirect
// watches the holder (its process and its exiting signal, both opened before
// anything is handed over), so a holder that ends first, or that is exiting,
// sends the launch to wait and register again rather than to an "already
// running" box.
func TestRelaunchDuringQuitALaunchWaitsForTheExitingInstance(t *testing.T) {
	main := appMainSource(t)
	entry := definitionBody(t, "main.cpp", main, "int __stdcall wWinMain(")
	signOutRequireInOrder(t, "wWinMain", entry,
		regexp.QuoteMeta("Launcher launcher(args, primary);"),
		regexp.QuoteMeta("urnw::instance::Launch(launcher);"),
		regexp.QuoteMeta("case urnw::instance::LaunchResult::Holder:"),
		regexp.QuoteMeta("break;"),
		regexp.QuoteMeta("case urnw::instance::LaunchResult::HandedOver:"),
		regexp.QuoteMeta("return 0;"),
		regexp.QuoteMeta("default:"),
		regexp.QuoteMeta("ReportLaunchFailure(launch, launcher.Failure(launch));"),
		regexp.QuoteMeta("::ExitProcess(1);"),
		regexp.QuoteMeta("Application::Start("))

	redirect := definitionBody(t, "main.cpp", main, "urnw::instance::RedirectAttempt Launcher::Redirect() {")
	signOutRequireInOrder(t, "Launcher::Redirect", redirect,
		regexp.QuoteMeta("watch_.emplace(holder_.ProcessId());"),
		regexp.QuoteMeta("if (watch_->Gone()) return {.result = RedirectResult::HolderGone, .holderExiting = true};"),
		regexp.QuoteMeta("holder.RedirectActivationToAsync(args).get();"),
		regexp.QuoteMeta("watch_->AwaitRedirect(state->done.get(), urnw::instance::kRedirectBudget, &waitFailure);"),
		regexp.QuoteMeta("return {.result = result, .holderExiting = watch_->Exiting()};"))
	// the redirect is never waited for without the holder's process beside it
	if strings.Contains(main, "CoWaitForMultipleObjects(") {
		t.Error("main.cpp waits on its own: the redirect must be waited for through HolderWatch, " +
			"beside the holder's process, or a holder that ends first reads as not responding")
	}
	launcher := quitSourceFrom(t, "main.cpp", main, "class Launcher {", "\n};\n")
	register := quitSourceFrom(t, "Launcher", launcher, "bool Register() {", "\n  }\n")
	signOutRequireInOrder(t, "Launcher::Register", register,
		regexp.QuoteMeta("watch_.reset();"),
		regexp.QuoteMeta("holder_ = AppInstance::FindOrRegisterForKey(kInstanceKey);"))
	provideRequire(t, "Launcher::AwaitHolderExit",
		quitSourceFrom(t, "Launcher", launcher, "bool AwaitHolderExit() {", "\n  }\n"),
		"watch_->AwaitExit(urnw::instance::kExitingHolderBudget)")

	// only an instance that still runs and is not exiting is "already running"
	report := definitionBody(t, "main.cpp", main,
		"void ReportLaunchFailure(urnw::instance::LaunchResult result, const std::wstring& detail) {")
	closing := quitSourceFrom(t, "ReportLaunchFailure", report, "case LaunchResult::StillClosing:", "return;")
	provideRequire(t, "ReportLaunchFailure's StillClosing case", closing, "URnetwork is still closing")
	quitForbid(t, "ReportLaunchFailure's StillClosing case", closing,
		"an exiting instance is not a running one", "already running")
	provideRequire(t, "ReportLaunchFailure's NoResponse case",
		quitSourceFrom(t, "ReportLaunchFailure", report, "case LaunchResult::NoResponse:", "return;"),
		"URnetwork is already running, but it did not respond.")

	glue := stripComments(readAppSource(t, "SingleInstance.cpp"))
	watch := definitionBody(t, "SingleInstance.cpp", glue, "HolderWatch::HolderWatch(std::uint32_t processId) {")
	signOutRequireInOrder(t, "HolderWatch::HolderWatch", watch,
		regexp.QuoteMeta("process_ = ::OpenProcess(SYNCHRONIZE, FALSE, processId);"),
		regexp.QuoteMeta("if (!process_) gone_ = ::GetLastError() == ERROR_INVALID_PARAMETER;"),
		regexp.QuoteMeta("exitingSignal_ = ::OpenEventW(SYNCHRONIZE, FALSE, ExitingSignalName(processId).c_str());"))
	provideRequire(t, "HolderWatch::AwaitRedirect",
		definitionBody(t, "SingleInstance.cpp", glue, "HolderWatch::RedirectWait HolderWatch::AwaitRedirect("),
		"HANDLE handles[] = {static_cast<HANDLE>(done), static_cast<HANDLE>(process_)};",
		"AwaitAny(handles, process_ ? 2 : 1, budget, &index);",
		"return index == 0 ? RedirectWait::Done : RedirectWait::HolderGone;")
	provideRequire(t, "HolderWatch::Exiting", glue,
		"bool HolderWatch::Exiting() const { return Signalled(exitingSignal_) || Signalled(process_); }")

	project := readAppSource(t, "App.vcxproj")
	provideRequire(t, "App.vcxproj", project,
		`<ClCompile Include="SingleInstance.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClInclude Include="SingleInstance.h" />`)
}
