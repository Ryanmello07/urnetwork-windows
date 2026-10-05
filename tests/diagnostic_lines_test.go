// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The service's diagnostic lines in the log "send feedback with logs" uploads
// (Common/DiagnosticLines.h): the formatters and classifiers run in
// app/tools/diagnostic-lines-tests.cpp against the production headers; the
// writer (Service/ServiceDiagnostics.cpp), its call sites and the app's proxy
// reader need Windows, so what they must keep doing is read off their sources,
// with comments stripped so prose cannot satisfy a contract.

// The headers the harness includes, all from app/src/Common.
var diagnosticLinesHeaders = []string{"DiagnosticLines.h", "NetworkCountry.h"}

// Builds the harness against a copy of the headers, `mutate` applied, and
// returns the program's path.
func diagnosticLinesTestProgram(t *testing.T, mutate map[string]func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("diagnostic lines tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range diagnosticLinesHeaders {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "Common", name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[name]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "diagnostic-lines-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "diagnostic-lines-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build diagnostic lines tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: every line's shape, its closed sets, the interface, DoH
// and proxy classifiers, and hostile input that must never come out.
func TestDiagnosticLines(t *testing.T) {
	program := diagnosticLinesTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("diagnostic lines: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each closed set opened, each classifier made to leak or lie.
func TestDiagnosticLinesRejectsLeaks(t *testing.T) {
	for _, control := range []struct {
		name string
		old  string
		new  string
		want string
	}{
		{
			name: "a firewall state written as sent",
			old:  `line += OneOf(f.firewall, {"off", "armed", "connecting", "connected"}, "other");`,
			new:  `line += f.firewall;`,
			want: "service: an unknown firewall state is other",
		},
		{
			name: "the app's provide mode written verbatim",
			old:  `line += OneOf(f.providerControlMode, {"never", "always", "network", "auto"}, "unknown");`,
			new:  `line += f.providerControlMode;`,
			want: "service: the app's provide mode is never written verbatim",
		},
		{
			name: "a LAN proxy taken for a public one",
			old:  "if (o[0] == 10 || ",
			new:  "if (",
			want: "host: 10.0.0.5 is private",
		},
		{
			name: "any proxy kind a peer sends",
			old:  "  if (kind == kProxyNone) return std::string(kProxyNone);\n",
			new:  "  return std::string(kind);\n",
			want: "proxy: a kind the app cannot produce is unknown",
		},
		{
			name: "credentials kept in a proxy entry",
			old:  "  if (const auto at = entry.rfind('@'); at != std::string_view::npos) entry = entry.substr(at + 1);\n",
			new:  "",
			want: "entry: credentials and a path are cut away",
		},
		{
			name: "no cap on the NRPT count",
			old:  "} else if (nrptRules > kNrptRulesCap) {",
			new:  "} else if (false) {",
			want: "dns: more than 99 rules are 99+",
		},
		{
			name: "a link that is down not said",
			old:  "  if (!f.connected) name += \"-down\";\n",
			new:  "",
			want: "adapter: a link that is down says so",
		},
		{
			name: "a log upload's carrier written as handed over",
			old:  `return "carrier=" + std::string(OneOf(carrier, {"tunnel", "provider", "standalone"}, "other"));`,
			new:  `return "carrier=" + std::string(carrier);`,
			want: "log upload: a carrier the service cannot choose is other",
		},
	} {
		program := diagnosticLinesTestProgram(t, map[string]func(string) string{
			"DiagnosticLines.h": replaceOnce(control.old, control.new)})
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s",
				control.name, control.want, err, output)
		}
	}
}

// A start_tunnel reader that drops the proxy kind: every connect would log
// "unknown".
func TestDiagnosticLinesRejectsDroppedProxyKind(t *testing.T) {
	requireNetworkCountryFailures(t, networkCountryProtocolHarness, []networkCountryControl{
		{
			name: "start_tunnel drops the proxy kind",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  get(\"system_proxy\", v.system_proxy);\n", "")},
			want: "pipe: start_tunnel carries the system proxy kind",
		},
	})
}

// The service writes at the RPC boundary: the pushed state's lines before the
// flush that takes them along, the proxy line before a start is served, the
// network country in force after set_network_country has applied it, and an
// upload's carrier through the hook upload_logs passes. Nothing else in the
// tree writes into the uploaded log.
func TestDiagnosticLinesServiceWiring(t *testing.T) {
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	push := definitionBody(t, "ControlServer.cpp", server, "void ControlServer::PushState()")
	provideRequireOrder(t, "PushState", push,
		"diagnostics_.NoteStatus(tunnel_.Status(), tunnel_.KillSwitchPreference(),", "urnet::flushGlog();")
	provideRequire(t, "PushState", push, "tunnel_.NetworkCountry());")
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	at := strings.Index(handle, "proto::msg::kStartTunnel")
	if at < 0 {
		t.Fatal("ControlServer::Handle does not answer start_tunnel")
	}
	provideRequireOrder(t, "the start_tunnel branch", handle[at:], "diagnostics_.NoteStart(cfg);", "tunnel_.Start(cfg)")
	// the identity check refuses before anything is written for a start
	provideRequireOrder(t, "the start_tunnel branch", handle[at:], "rpcsession::IsPairableInstanceId(", "diagnostics_.NoteStart(cfg);")
	at = strings.Index(handle, "proto::msg::kSetNetworkCountry")
	if at < 0 {
		t.Fatal("ControlServer::Handle does not answer set_network_country")
	}
	provideRequireOrder(t, "the set_network_country branch", handle[at:],
		"tunnel_.SetNetworkCountry(s.network_country_code, s.network_country_source);",
		"diagnostics_.NoteNetworkCountry(tunnel_.NetworkCountry());")
	at = strings.Index(handle, "proto::msg::kUploadLogs")
	if at < 0 {
		t.Fatal("ControlServer::Handle does not answer upload_logs")
	}
	provideRequireOrder(t, "the upload_logs branch", handle[at:],
		"tunnel_.UploadLogs(req, error, carrier, [this](std::string_view chosen) {",
		"diagnostics_.NoteLogUpload(chosen);")
	header := stripComments(readServiceSource(t, "ControlServer.h"))
	// declared first, destroyed last: the pipe and the tunnel push into it
	provideRequireOrder(t, "ControlServer.h", header, "ServiceDiagnostics diagnostics_;", "TunnelController tunnel_;")
	provideRequireOrder(t, "ControlServer.h", header, "ServiceDiagnostics diagnostics_;", "PipeServer pipe_;")

	writer := stripComments(readServiceSource(t, "ServiceDiagnostics.cpp"))
	note := definitionBody(t, "ServiceDiagnostics.cpp", writer, "void ServiceDiagnostics::NoteStatus(")
	provideRequire(t, "NoteStatus", note,
		"diag::ServiceLine(facts)",
		"diag::AdapterLine(status.routes_installed, InterfaceFactsFor(status.egress_index4)",
		"InterfaceFactsFor(status.egress_index6)",
		"diag::DohPolicyFor(ReadPolicyDword(kDnsClientPolicyKey, L\"DoHPolicy\"))",
		"CountNrptRules()",
		"std::scoped_lock lock(mutex_);")
	// the DNS settings and the country are said while a session is live, and
	// forgotten when it ends, so the next session says them again
	live := strings.Index(note, "if (proto::IsSessionLive(status.state)) {")
	ended := strings.Index(note, "} else {")
	if live < 0 || ended < live {
		t.Fatal("NoteStatus must write the DNS and network country lines in a live-session branch")
	}
	provideRequire(t, "NoteStatus's live branch", note[live:ended], "diag::kTagDns", "WriteNetworkCountryLocked(networkCountry);")
	provideRequire(t, "NoteStatus's ended branch", note[ended:], "lastDns_.clear();", "lastNetworkCountry_.clear();")
	start := definitionBody(t, "ServiceDiagnostics.cpp", writer, "void ServiceDiagnostics::NoteStart(")
	provideRequire(t, "NoteStart", start, "diag::ProxyLine(request.system_proxy)")
	upload := definitionBody(t, "ServiceDiagnostics.cpp", writer, "void ServiceDiagnostics::NoteLogUpload(")
	provideRequireOrder(t, "NoteLogUpload", upload, "std::scoped_lock lock(mutex_);",
		"diag::LogUploadLine(carrier)")
	country := definitionBody(t, "ServiceDiagnostics.cpp", writer, "void ServiceDiagnostics::WriteNetworkCountryLocked(")
	provideRequireOrder(t, "WriteNetworkCountryLocked", country, "if (!networkCountry) return;",
		"diag::NetworkCountryLine(networkCountry->code, networkCountry->source)")
	write := definitionBody(t, "ServiceDiagnostics.cpp", writer, "void ServiceDiagnostics::WriteIfChangedLocked(")
	provideRequireOrder(t, "WriteIfChangedLocked", write, "if (line == last) return;", "urnet::logAppInfo(")
	// the interface is read for its kind and nothing else of it
	requireNone(t, "ServiceDiagnostics.cpp", writer, "Alias", "Description", "FriendlyName",
		"PhysicalAddress", "InterfaceGuid", "DescribeInterface(", "HostResolversV4(", "lock(tunnel")

	tunnel := tunnelControllerSource(t)
	read := definitionBody(t, "TunnelController.cpp", tunnel,
		"std::optional<netcountry::Reading> TunnelController::NetworkCountry()")
	provideRequire(t, "TunnelController::NetworkCountry", read, "std::scoped_lock lock(networkCountryMutex_);")
	requireNone(t, "TunnelController::NetworkCountry", read, "lock(mutex_", "mutex_, std::defer_lock")
	controllerHeader := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", controllerHeader,
		"bool KillSwitchPreference() const { return killSwitch_.load(); }")

	// Every line in the uploaded log comes from ServiceDiagnostics, under one of
	// the six tags.
	logAppInfo := regexp.MustCompile(`logAppInfo\(`)
	for _, dir := range []string{"App", "Service", "Common"} {
		entries, err := os.ReadDir(filepath.Join(repositoryRoot(t), "app", "src", dir))
		if err != nil {
			t.Fatal(err)
		}
		for _, entry := range entries {
			name := entry.Name()
			if entry.IsDir() || !(strings.HasSuffix(name, ".cpp") || strings.HasSuffix(name, ".h")) {
				continue
			}
			data, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", dir, name))
			if err != nil {
				t.Fatal(err)
			}
			count := len(logAppInfo.FindAllStringIndex(stripComments(string(data)), -1))
			want := 0
			if dir+"/"+name == "Service/ServiceDiagnostics.cpp" {
				want = 3
			}
			if count != want {
				t.Errorf("%s/%s calls logAppInfo %d times, want %d: the uploaded log takes only "+
					"Common/DiagnosticLines.h's lines, from ServiceDiagnostics", dir, name, count, want)
			}
		}
	}
	for _, line := range []string{
		"urnet::logAppInfo(std::string(diag::kTagProxy), diag::ProxyLine(request.system_proxy));",
		"urnet::logAppInfo(std::string(tag), line);",
		"urnet::logAppInfo(std::string(diag::kTagLogUpload), diag::LogUploadLine(carrier));",
	} {
		provideRequire(t, "ServiceDiagnostics.cpp", writer, line)
	}
}

// The app reads the user's proxy setting for its kind, frees what it read,
// logs none of it, and sends the kind with the start.
func TestDiagnosticLinesAppWiring(t *testing.T) {
	reader := stripComments(readAppSource(t, "SystemProxy.cpp"))
	body := definitionBody(t, "SystemProxy.cpp", reader, "std::string ReadUserProxyKind()")
	provideRequire(t, "ReadUserProxyKind", body,
		"::WinHttpGetIEProxyConfigForCurrentUser(&config)",
		"diag::UserProxyKind(config.fAutoDetect != FALSE, pacScript, proxyList)",
		"::GlobalFree(config.lpszAutoConfigUrl);",
		"::GlobalFree(config.lpszProxy);",
		"::GlobalFree(config.lpszProxyBypass);")
	requireNone(t, "SystemProxy.cpp", reader, "LogInfo(", "LogWarn(", "LogError(", "logAppInfo(",
		"#include \"pch.h\"")

	host := sdkHostSource(t)
	bootstrap := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::BootstrapSession(")
	provideRequireOrder(t, "BootstrapSession", bootstrap,
		"cfg.system_proxy = ReadUserProxyKind();", "service_.StartTunnel(cfg)")

	app := readAppSource(t, "App.vcxproj")
	provideRequire(t, "App.vcxproj", app,
		`<ClCompile Include="SystemProxy.cpp"><PrecompiledHeader>NotUsing</PrecompiledHeader></ClCompile>`,
		`<ClInclude Include="SystemProxy.h" />`)
	service := readServiceSource(t, "Service.vcxproj")
	provideRequire(t, "Service.vcxproj", service,
		`<ClCompile Include="ServiceDiagnostics.cpp" />`, `<ClInclude Include="ServiceDiagnostics.h" />`)
	common := readCommonSource(t, "Common.vcxproj")
	provideRequire(t, "Common.vcxproj", common, `<ClInclude Include="DiagnosticLines.h" />`)
	header := readCommonSource(t, "DiagnosticLines.h")
	if regexp.MustCompile(`#include\s*<(windows|winsock2|winhttp)\.h>|#include\s*"(Sdk|Log|pch|Protocol)\.h"`).MatchString(header) {
		t.Error("DiagnosticLines.h must stay portable: its harness runs off Windows")
	}
}
