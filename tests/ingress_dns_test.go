// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The network space's ingress dns on Windows -- the connection region and the
// bootstrap DNS-over-HTTPS servers (sdk control_doh.go) -- reaches the
// service's running session, not only the app (support inbox 8051, 1135,
// 7861). The service runs the session's device and the provider-only device in
// a copy of the space imported at start_tunnel and start_provider, and kept the
// values it imported, so a region chosen from the "Can't reach URnetwork"
// prompt over a stalled session never reached that session until the user
// reconnected. Now every save, and every reattach to a running session, hands
// the service the values the app's space holds (set_ingress_dns), and the
// service writes them into its space, where they apply in place, as they do
// for an in-process tunnel. The pure parts run in
// app/tools/ingress-dns-tests.cpp against the production sources; SdkHost,
// ServiceClient and the service need Windows (and the app WinRT), so what they
// must keep doing is read off their sources, with comments stripped so prose
// cannot satisfy a contract.

// The sources the harness compiles, by their directory under app/src. They are
// copied into a fixture tree, so `mutate`, keyed by file name, can rewrite one
// for a negative control.
var ingressDnsSourceDirs = map[string]string{
	"Protocol.h":         "Common",
	"ProvideLifecycle.h": "Common",
	"ExtenderReset.h":    "Common",
	"IngressDnsSync.h":   "Common",
}

// Builds the harness against a fixture copy of the sources, `mutate` applied,
// and returns the program's path. `extra` adds compiler flags.
func ingressDnsTestProgram(t *testing.T, mutate map[string]func(string) string,
	extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("ingress dns tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixture := t.TempDir()
	for name, dir := range ingressDnsSourceDirs {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", dir, name))
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
		if err := os.MkdirAll(filepath.Join(fixture, dir), 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(fixture, dir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixture, "ingress-dns-tests")
	args := []string{"-std=c++20", "-pthread", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(fixture, "Common")}
	args = append(args, provideJsonFlags(t)...)
	args = append(args, extra...)
	args = append(args, filepath.Join(root, "app", "tools", "ingress-dns-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build ingress-dns-tests.cpp: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the verb, its strict request, the reply and an older
// service's silence, the key from the app's space to the service's lookup, and
// the re-send a busy service is owed.
func TestIngressDns(t *testing.T) {
	program := ingressDnsTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("ingress dns: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The same spec on the generated header's own urnet::NetworkSpaceKey, with the
// values read through its space values; and the header declares every call
// SdkHost and the service make for the values. The header is git-ignored
// (fetch-deps unpacks it into app/third_party/urnetwork-sdk/<arch>;
// URNETWORK_SDK_INCLUDE names another directory), so a host without one skips
// this, and so does a copy from before the connection region.
func TestIngressDnsAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "setIngressCountryCode(")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the connection region (set URNETWORK_SDK_INCLUDE)")
	}
	data, err := os.ReadFile(filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	if err != nil {
		t.Fatal(err)
	}
	header := string(data)
	for _, want := range []string{
		"std::string getIngressCountryCode() const;",
		"std::string setIngressCountryCode(const std::string& country_code) const;",
		"std::optional<StringList> getControlDohUrls() const;",
		"std::string setControlDohUrls(const std::optional<StringList>& doh_urls) const;",
		"std::optional<NetworkSpaceKey> getKey() const;",
		"NetworkSpace getNetworkSpace(const std::optional<NetworkSpaceKey>& key) const;",
		"std::optional<std::string> ingress_country_code;",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	program := ingressDnsTestProgram(t, nil, "-DURNW_INGRESS_DNS_TESTS_SDK", "-isystem", sdkDir)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("ingress dns against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("against %s\n%s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"), output)
	}
}

// One negative control: a rewrite of the production sources, and the failure
// line the harness must print for it.
type ingressDnsControl struct {
	name   string
	mutate map[string]func(string) string
	want   string
}

// Runs the harness against each control's rewritten sources and reports, by
// name, every control it does not fail on with the wanted line.
func requireIngressDnsFailures(t *testing.T, controls []ingressDnsControl) {
	t.Helper()
	for _, control := range controls {
		program := ingressDnsTestProgram(t, control.mutate)
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s",
				control.name, control.want, err, output)
		}
	}
}

// A lenient request writes a space it does not name or a value nobody chose,
// a reply that loses `ingress_dns_held` or `ingress_dns_busy` hides what the
// service did, and a key rule that sends what the service must refuse, or
// names another space, writes the wrong one.
func TestIngressDnsRejectsAWrongWire(t *testing.T) {
	requireIngressDnsFailures(t, []ingressDnsControl{
		{
			name: "an empty key accepted",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {\n      throw std::runtime_error(std::string(\"set_ingress_dns requires \") + k +",
				"if (it == j.end() || !it->is_string()) {\n      throw std::runtime_error(std::string(\"set_ingress_dns requires \") + k +")},
			want: "wire: a request with host_name empty is refused",
		},
		{
			name: "a missing region read as undisclosed",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  if (region == j.end() || !region->is_string()) {\n    throw std::runtime_error(\"set_ingress_dns requires ingress_country_code (a string)\");\n  }\n  v.ingress_country_code = region->get<std::string>();",
				"  if (region != j.end() && region->is_string()) v.ingress_country_code = region->get<std::string>();")},
			want: "wire: a request with ingress_country_code missing is refused",
		},
		{
			name: "missing servers read as the built-in servers",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  if (urls == j.end() || !urls->is_array()) {\n    throw std::runtime_error(\"set_ingress_dns requires control_doh_urls (an array of strings)\");\n  }",
				"  if (urls == j.end() || !urls->is_array()) return;")},
			want: "wire: a request with control_doh_urls missing is refused",
		},
		{
			name: "a request without its servers",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"       {\"control_doh_urls\", v.control_doh_urls}};", "       {\"control_doh_urls\", nlohmann::json::array()}};")},
			want: "wire: the request round-trips its key, its region and its servers",
		},
		{
			name: "the reply's held not read",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  get(\"ingress_dns_held\", v.ingress_dns_held);\n", "")},
			want: "reply: ingress_dns_held round-trips",
		},
		{
			name: "the reply's held not written",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  if (v.ingress_dns_held) j[\"ingress_dns_held\"] = true;\n", "")},
			want: "reply: ingress_dns_held round-trips",
		},
		{
			name: "the reply's busy not read",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  get(\"ingress_dns_busy\", v.ingress_dns_busy);\n", "")},
			want: "reply: ingress_dns_busy round-trips",
		},
		{
			name: "a request sent without an env",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"  if (request.host_name.empty() || request.env_name.empty()) return std::nullopt;\n  request.ingress_country_code = ingressCountryCode;",
				"  if (request.host_name.empty()) return std::nullopt;\n  request.ingress_country_code = ingressCountryCode;")},
			want: "keys: an empty env sends nothing",
		},
		{
			name: "the env looked up as the host",
			mutate: map[string]func(string) string{"Protocol.h": replaceOnce(
				"Key SpaceKeyOf(const SetIngressDns& request) {\n  Key key;\n  key.host_name = request.host_name;\n  key.env_name = request.env_name;",
				"Key SpaceKeyOf(const SetIngressDns& request) {\n  Key key;\n  key.host_name = request.host_name;\n  key.env_name = request.host_name;")},
			want: "keys: the host and the env each go to their own field",
		},
	})
}

// A re-send taken during a transition goes while the operation still holds
// the service's lock, one that stays owed goes again at every status, and one
// that newer taken values do not clear writes older values over them.
func TestIngressDnsRejectsAWrongResend(t *testing.T) {
	requireIngressDnsFailures(t, []ingressDnsControl{
		{
			name: "due during a transition",
			mutate: map[string]func(string) string{"IngressDnsSync.h": replaceOnce(
				"    if (!extenderreset::EndsOperation(pushed)) return std::nullopt;\n", "    (void)pushed;\n")},
			want: "owed: a transition's status does not end the operation",
		},
		{
			name: "due more than once",
			mutate: map[string]func(string) string{"IngressDnsSync.h": replaceOnce(
				"return std::exchange(owed_, std::nullopt);", "return owed_;")},
			want: "owed: due once",
		},
		{
			name: "taken values leave older ones owed",
			mutate: map[string]func(string) string{"IngressDnsSync.h": replaceOnce(
				"    } else {\n      owed_.reset();\n    }\n", "    }\n")},
			want: "owed: newer values that were taken owe nothing",
		},
	})
}

// The service answers the verb on the one control pipe and writes the values
// through the space's own setters into the space the manager holds for the
// request's key -- the one object the session's device and the provider-only
// device run in -- looking it up under the session lock in budget and writing
// with the lock released.
func TestIngressDnsServiceWiring(t *testing.T) {
	controller := stripComments(readServiceSource(t, "TunnelController.h"))
	provideRequire(t, "TunnelController.h", controller,
		"IngressDnsResult SetIngressDns(const proto::SetIngressDns& request);")

	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	start := definitionBody(t, "ControlServer.cpp", server, "bool ControlServer::Start()")
	provideRequire(t, "ControlServer::Start", start, "return Handle(req);")
	handle := definitionBody(t, "ControlServer.cpp", server, "nlohmann::json ControlServer::Handle(")
	if !strings.Contains(handle, "type == proto::msg::kSetIngressDns") {
		t.Fatal("ControlServer::Handle does not answer set_ingress_dns: a region saved in the app " +
			"never reaches the space the service's running session resolves through")
	}
	branch := sourceBetween(t, "ControlServer::Handle", handle,
		"type == proto::msg::kSetIngressDns", "} else {")
	provideRequireOrder(t, "the set_ingress_dns branch", branch,
		"request.get<proto::SetIngressDns>()",
		"const TunnelController::IngressDnsResult result = tunnel_.SetIngressDns(req);")
	provideRequire(t, "the set_ingress_dns branch", branch, "reply.ok = result.ok;",
		"reply.ingress_dns_held = result.held;", "reply.ingress_dns_busy = result.busy;",
		"reply.error = result.error;")
	// the tunnel's status does not move
	requireNone(t, "the set_ingress_dns branch", branch, "PushState()")

	source := tunnelControllerSource(t)
	write := definitionBody(t, "TunnelController.cpp", source,
		"TunnelController::IngressDnsResult TunnelController::SetIngressDns(")
	provideRequire(t, "TunnelController::SetIngressDns", write,
		"proto::SpaceKeyOf<urnet::NetworkSpaceKey>(request)",
		"request.host_name", "request.env_name")
	locked := lockBlock(t, "TunnelController::SetIngressDns", write,
		"std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);")
	// a wedged bring-up refuses it in budget rather than holding the pipe
	provideRequireOrder(t, "the session lock's block", locked,
		"lock.try_lock_for(kStopLockBudget)", "spaceManager_->getNetworkSpace(key)")
	refused := sourceBetween(t, "the session lock's block", locked,
		"lock.try_lock_for(kStopLockBudget)", "LogWarn(")
	provideRequire(t, "the refusal", refused, "result.busy = true;",
		"result.error = \"a tunnel operation is in progress\";")
	provideRequire(t, "the session lock's block", locked,
		"if (spaceManager_) space = spaceManager_->getNetworkSpace(key);")
	// the setters swap the space's DoH cache, which joins its lookups: never
	// under the lock
	requireNone(t, "the session lock's block", locked, "setIngressCountryCode", "setControlDohUrls")
	provideRequireOrder(t, "TunnelController::SetIngressDns", write, locked,
		"space.setIngressCountryCode(request.ingress_country_code)")
	provideRequireOrder(t, "TunnelController::SetIngressDns", write, "if (space) {",
		"space.setIngressCountryCode(request.ingress_country_code)")
	provideRequireOrder(t, "TunnelController::SetIngressDns", write,
		"space.setIngressCountryCode(request.ingress_country_code)",
		"space.setControlDohUrls(urnet::StringList(request.control_doh_urls))")
	// only a lock that was not free is busy: a failed lookup or write is not
	if count := strings.Count(write, "result.busy = true;"); count != 1 {
		t.Errorf("TunnelController::SetIngressDns sets busy %d times; only the lock's refusal may", count)
	}
	// one write through the manager's space covers both devices; a write to one
	// device's slot would miss the other
	requireNone(t, "TunnelController::SetIngressDns", write, "networkSpace_", "providerSpace_")
	// where a user connects from and which resolvers they reach stay theirs
	for _, line := range logCalls(write) {
		requireNone(t, "a log line of TunnelController::SetIngressDns", line,
			"ingress_country_code", "control_doh_urls")
	}
}

// The app hands the service the values its space holds after every save of
// the region or the servers, whether or not a session runs, and after it
// reattaches to a running session; the send leaves mutex_ for the
// provider-only worker, and a busy refusal is owed until a pushed status ends
// the operation that held the service's lock.
func TestIngressDnsAppWiring(t *testing.T) {
	host := sdkHostSource(t)

	region := definitionBody(t, "SdkHost.cpp", host,
		"std::optional<std::string> SdkHost::SetIngressCountryCode(")
	if !strings.Contains(region, "QueueIngressDnsLocked(") {
		t.Fatal("SdkHost::SetIngressCountryCode saves the region in the app's space only: a " +
			"session the service is running (the one the can't-reach prompt is raised over) " +
			"keeps resolving through the region it imported at its start")
	}
	provideRequireOrder(t, "SdkHost::SetIngressCountryCode", region,
		"networkSpace_->setIngressCountryCode(code)", "QueueIngressDnsLocked(")
	requireNotGatedOnNoSession(t, "SdkHost::SetIngressCountryCode", region)

	servers := definitionBody(t, "SdkHost.cpp", host,
		"std::optional<std::string> SdkHost::SetControlDohUrls(")
	saved := sourceBetween(t, "SdkHost::SetControlDohUrls", servers, "if (errorId.empty()) {", "return errorId;")
	if !strings.Contains(saved, "QueueIngressDnsLocked(") {
		t.Fatal("SdkHost::SetControlDohUrls saves the servers in the app's space only: the " +
			"service's running session keeps the servers it imported at its start")
	}
	requireNotGatedOnNoSession(t, "SdkHost::SetControlDohUrls", saved)

	bootstrap := definitionBody(t, "SdkHost.cpp", host,
		"bool SdkHost::BootstrapSession(")
	if !strings.Contains(bootstrap, "if (reattaching) QueueIngressDnsLocked(") {
		t.Fatal("SdkHost::BootstrapSession reattaches to a running session without handing it " +
			"the values the app's space holds: it keeps the region it imported at its start")
	}
	provideRequireOrder(t, "SdkHost::BootstrapSession", bootstrap,
		"device_ = urnet::newDeviceRemoteWithDefaults(", "if (reattaching) QueueIngressDnsLocked(")

	queue := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::QueueIngressDnsLocked(")
	provideRequire(t, "SdkHost::QueueIngressDnsLocked", queue,
		"proto::SetIngressDnsRequestFor(", "networkSpace_->getKey()",
		"networkSpace_->getIngressCountryCode()", "networkSpace_->getControlDohUrls()")
	queued := lockBlock(t, "SdkHost::QueueIngressDnsLocked", queue,
		"std::scoped_lock lock(providerOnlyMutex_);")
	provideRequire(t, "the queue's block", queued, "ingressDnsSend_ = std::move(request);",
		"providerOnlyKick_ = true;")
	// queued under mutex_, sent off it: the pipe call can wait behind a start
	requireNone(t, "SdkHost::QueueIngressDnsLocked", queue, "service_.")

	loop := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ProviderOnlyStatsLoop(")
	provideRequireOrder(t, "SdkHost::ProviderOnlyStatsLoop", loop,
		"ingressDns = std::exchange(ingressDnsSend_, std::nullopt);",
		"if (ingressDns) SendIngressDns(*ingressDns);")

	send := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SendIngressDns(")
	provideRequireOrder(t, "SdkHost::SendIngressDns", send,
		"service_.SetIngressDns(request, &held, &error)", "owedIngressDns_.Answered(request, answer);")
	for _, line := range logCalls(send + queue) {
		requireNone(t, "a log line of the app's ingress dns send", line,
			"ingress_country_code", "control_doh_urls", "getIngressCountryCode", "getControlDohUrls")
	}

	resend := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::QueueIngressDnsResend(")
	provideRequire(t, "SdkHost::QueueIngressDnsResend", resend,
		"if (!ingressDnsSend_) ingressDnsSend_ = std::move(request);")
	initialize := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize(")
	handler := handlerSource(t, "SdkHost.cpp", initialize, "service_.SetStateHandler(")
	provideRequire(t, "the service's state handler", handler,
		"if (auto due = owedIngressDns_.TakeDue(st.state)) {", "QueueIngressDnsResend(std::move(*due));")

	clientHeader := stripComments(readAppSource(t, "ServiceClient.h"))
	provideRequire(t, "ServiceClient.h", clientHeader,
		"ingressdns::ServiceAnswer SetIngressDns(const proto::SetIngressDns& request,")
	client := stripComments(readAppSource(t, "ServiceClient.cpp"))
	call := definitionBody(t, "ServiceClient.cpp", client,
		"ingressdns::ServiceAnswer ServiceClient::SetIngressDns(")
	provideRequire(t, "ServiceClient::SetIngressDns", call,
		"nlohmann::json body = request;", "proto::msg::kSetIngressDns",
		"*held = r.ingress_dns_held;", "r.ingress_dns_busy ? ingressdns::ServiceAnswer::Busy")
}

// A send that only goes when no session runs misses the very session the
// can't-reach prompt is raised over.
func requireNotGatedOnNoSession(t *testing.T, where, body string) {
	t.Helper()
	for _, line := range strings.Split(body, "\n") {
		if strings.Contains(line, "QueueIngressDnsLocked(") &&
			(strings.Contains(line, "device_") || strings.Contains(line, "HasSession()")) {
			t.Errorf("%s gates the ingress dns on the session: %s", where, strings.TrimSpace(line))
		}
	}
}

// The Log* calls of source, each from its name through the end of its
// statement.
func logCalls(source string) []string {
	calls := []string{}
	for _, opener := range []string{"LogInfo(", "LogWarn(", "LogError("} {
		rest := source
		for {
			at := strings.Index(rest, opener)
			if at < 0 {
				break
			}
			end := strings.Index(rest[at:], ");")
			if end < 0 {
				calls = append(calls, rest[at:])
				break
			}
			calls = append(calls, rest[at:at+end+2])
			rest = rest[at+end+2:]
		}
	}
	return calls
}
