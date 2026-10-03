// SPDX-License-Identifier: MPL-2.0

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"
)

// ---- pure decisions ----

func heldGuest() guestObservation {
	return guestObservation{
		Uia: uiaFacts{Window: true, AutomationIdFacts: map[string]elementFacts{
			idConnect: {Present: true, Enabled: true, Name: labelDisconnect},
			idAlert:   {Present: true, Enabled: true, Name: "Insufficient balance"},
			idUpgrade: {Present: true, Enabled: true, Name: "Get Pro"},
		}},
		Prefixes: []string{"0.0.0.0/1", "128.0.0.0/1"},
		Notices:  3,
	}
}

func TestObservationFromGuest(t *testing.T) {
	withConnect := func(f elementFacts) guestObservation {
		g := heldGuest()
		g.Uia.AutomationIdFacts[idConnect] = f
		return g
	}
	cases := []struct {
		name  string
		guest guestObservation
		want  observation
	}{
		{"held", heldGuest(), observation{ConnectRequested: true, Connected: true, Alert: true, DisconnectButton: true, UpgradeButton: true, Notifications: 1}},
		{"disconnected", guestObservation{
			Uia:     uiaFacts{Window: true, AutomationIdFacts: map[string]elementFacts{idConnect: {Present: true, Enabled: true, Name: labelConnect}}},
			Notices: 2,
		}, observation{}},
		{"retry holds the destination but is not Disconnect", withConnect(elementFacts{Present: true, Enabled: true, Name: labelRetry}),
			observation{ConnectRequested: true, Connected: true, Alert: true, UpgradeButton: true, Notifications: 1}},
		{"disabled Disconnect is not usable", withConnect(elementFacts{Present: true, Name: labelDisconnect}),
			observation{ConnectRequested: true, Connected: true, Alert: true, UpgradeButton: true, Notifications: 1}},
		{"offscreen Disconnect is not visible", withConnect(elementFacts{Present: true, Offscreen: true, Enabled: true, Name: labelDisconnect}),
			observation{ConnectRequested: true, Connected: true, Alert: true, UpgradeButton: true, Notifications: 1}},
		{"default route", func() guestObservation { g := heldGuest(); g.Prefixes = []string{"0.0.0.0/0"}; return g }(),
			observation{ConnectRequested: true, Connected: true, Alert: true, DisconnectButton: true, UpgradeButton: true, Notifications: 1}},
		{"adapter routes that capture nothing", func() guestObservation {
			g := heldGuest()
			g.Prefixes = []string{"100.64.0.0/10", "224.0.0.0/4"}
			return g
		}(),
			observation{ConnectRequested: true, Alert: true, DisconnectButton: true, UpgradeButton: true, Notifications: 1}},
		{"closed banner", func() guestObservation {
			g := heldGuest()
			delete(g.Uia.AutomationIdFacts, idAlert)
			delete(g.Uia.AutomationIdFacts, idUpgrade)
			return g
		}(), observation{ConnectRequested: true, Connected: true, DisconnectButton: true, Notifications: 1}},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := observationFromGuest(c.guest, 2)
			if err != nil {
				t.Fatal(err)
			}
			if got != c.want {
				t.Fatalf("got %+v, want %+v", got, c.want)
			}
		})
	}
}

func TestObservationFromGuestFailures(t *testing.T) {
	noWindow := heldGuest()
	noWindow.Uia.Window = false
	noConnect := heldGuest()
	delete(noConnect.Uia.AutomationIdFacts, idConnect)
	lostLog := heldGuest()
	lostLog.Notices = 1
	for name, g := range map[string]guestObservation{"no window": noWindow, "no connect button": noConnect, "log lost lines": lostLog} {
		if _, err := observationFromGuest(g, 2); err == nil {
			t.Errorf("%s: want an error, never a guessed observation", name)
		}
	}
}

func TestEgressFromGuest(t *testing.T) {
	cases := []struct {
		guest guestEgress
		want  egressResult
	}{
		{guestEgress{Ip: "203.0.113.7\n"}, egressResult{Ip: "203.0.113.7"}},
		{guestEgress{Ip: "2001:db8::1"}, egressResult{Ip: "2001:db8::1"}},
		// a captive or error page is not an address, and is never reported as one
		{guestEgress{Ip: "<html>blocked</html>"}, egressResult{Error: "invalid public address response"}},
		// a held probe: curl times out inside the tunnel
		{guestEgress{Error: "https://checkip.amazonaws.com/=curl 28; https://api.ipify.org/=curl 28"},
			egressResult{Error: "https://checkip.amazonaws.com/=curl 28; https://api.ipify.org/=curl 28"}},
		{guestEgress{}, egressResult{Error: "no response"}},
	}
	for _, c := range cases {
		if got := egressFromGuest(c.guest); got != c.want {
			t.Errorf("%+v: got %+v, want %+v", c.guest, got, c.want)
		}
	}
	// the runner's leak decision compares ip against the direct address; a
	// held probe must therefore carry no ip at all
	b, _ := json.Marshal(egressFromGuest(guestEgress{Error: "curl 28"}))
	if string(b) != `{"error":"curl 28"}` {
		t.Fatalf("held probe JSON = %s", b)
	}
}

func TestParseGuestResult(t *testing.T) {
	got, err := parseGuestResult("noise\r\nIBRESULT {\"ip\":\"203.0.113.7\"}\r\nmore\r\n")
	if err != nil || string(got) != `{"ip":"203.0.113.7"}` {
		t.Fatalf("got %s, %v", got, err)
	}
	for name, stdout := range map[string]string{
		"missing":    "ok\n",
		"twice":      "IBRESULT {}\nIBRESULT {}\n",
		"not object": "IBRESULT [1]\n",
		"not json":   "IBRESULT {\n",
		"empty":      "IBRESULT \n",
	} {
		if _, err := parseGuestResult(stdout); err == nil {
			t.Errorf("%s: want an error", name)
		}
	}
}

func TestGuestCommand(t *testing.T) {
	got, err := guestCommand("interactive", "Op", "invoke", "Arg", idConnect+"|"+labelDisconnect)
	want := `powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File C:/acceptance-ib/guest.ps1 -Verb interactive -Op "invoke" -Arg "acceptance.connect|Disconnect"`
	if err != nil || got != want {
		t.Fatalf("got %q, %v", got, err)
	}
	for _, bad := range []string{"a b", `a"b`, "a;b", "$(x)", "a`b", "", "a'b"} {
		if _, err := guestCommand("observe", "Arg", bad); err == nil {
			t.Errorf("%q reached the guest command line", bad)
		}
	}
	if _, err := guestCommand("observe; rm", "Arg", "x"); err == nil {
		t.Error("unsafe verb accepted")
	}
	if _, err := guestCommand("observe", "Arg"); err == nil {
		t.Error("unpaired parameter accepted")
	}
}

func TestParseCredentials(t *testing.T) {
	c, err := parseCredentials(strings.NewReader("# acceptance\nemail: ib@example.com\npassword: 'p+a{s}s'\n"))
	if err != nil {
		t.Fatal(err)
	}
	if c.Email != "ib@example.com" || c.Password != "p+a{s}s" || c.PasswordSendKeys != "p{+}a{{}s{}}s" {
		t.Fatalf("got %+v", c)
	}
	for name, text := range map[string]string{
		"missing password": "email: a@b\n",
		"duplicate":        "email: a@b\nemail: c@d\npassword: x\n",
		"unknown key":      "email: a@b\npassword: x\ntoken: y\n",
		"no colon":         "email a@b\npassword: x\n",
	} {
		if _, err := parseCredentials(strings.NewReader(text)); err == nil {
			t.Errorf("%s: want an error", name)
		}
	}
}

func TestSendKeysEscape(t *testing.T) {
	if got := sendKeysEscape("a+^%~()[]{}z"); got != "a{+}{^}{%}{~}{(}{)}{[}{]}{{}{}}z" {
		t.Fatalf("got %q", got)
	}
}

func TestReadCredentialsRejectsReadableFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "c.yml")
	if err := os.WriteFile(path, []byte("email: a@b\npassword: secret-value\n"), 0644); err != nil {
		t.Fatal(err)
	}
	_, err := readCredentials(path)
	if err == nil || strings.Contains(err.Error(), "secret-value") {
		t.Fatalf("got %v", err)
	}
}

func TestRedactedLine(t *testing.T) {
	got := redactedLine("login\nfailed for ib@example.com with  hunter2", "hunter2", "ib@example.com")
	if got != "login failed for [redacted] with [redacted]" {
		t.Fatalf("got %q", got)
	}
}

func TestParseBootLine(t *testing.T) {
	st, err := parseBootLine("4242 /var/folders/x/T/tmp.AbC /var/folders/x/T/tmp.AbC/mon.sock 2222\n")
	if err != nil || st.QemuPid != 4242 || st.RunDir != "/var/folders/x/T/tmp.AbC" || st.SshPort != "2222" {
		t.Fatalf("got %+v, %v", st, err)
	}
	for _, bad := range []string{"", "0 /tmp/tmp.a /tmp/tmp.a/m 2222", "1 / /m 2222", "1 relative/tmp.a m 2222"} {
		if _, err := parseBootLine(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}

// ---- contracts with the app and the guest scripts ----

func repositoryRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", ".."))
	if err != nil {
		t.Fatal(err)
	}
	return root
}

// The labels main.go reads requestedness from are the en resources the VM
// shows.
func TestButtonLabelsMatchEnResources(t *testing.T) {
	b, err := os.ReadFile(filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "en", "Resources.resw"))
	if err != nil {
		t.Fatal(err)
	}
	var resources struct {
		Data []struct {
			Name  string `xml:"name,attr"`
			Value string `xml:"value"`
		} `xml:"data"`
	}
	if err := xml.Unmarshal(b, &resources); err != nil {
		t.Fatal(err)
	}
	nameValues := map[string]string{}
	for _, d := range resources.Data {
		nameValues[d.Name] = d.Value
	}
	for name, label := range map[string]string{"connect": labelConnect, "disconnect": labelDisconnect, "retry": labelRetry} {
		if nameValues[name] != label {
			t.Errorf("Resources.resw %s = %q, driver expects %q", name, nameValues[name], label)
		}
	}
}

// Every automation id the driver uses is set by the app, and the scripts use
// the same literals as main.go.
func TestAutomationIdsMatchAppAndScripts(t *testing.T) {
	root := repositoryRoot(t)
	read := func(parts ...string) string {
		b, err := os.ReadFile(filepath.Join(append([]string{root}, parts...)...))
		if err != nil {
			t.Fatal(err)
		}
		return string(b)
	}
	app := read("app", "src", "App", "MainWindow.xaml") + read("app", "src", "App", "MainWindow.xaml.cpp") +
		read("app", "src", "App", "SettingsPage.cpp")
	guest := read("tests", "insufficientbalance", "guest.ps1")
	uia := read("tests", "insufficientbalance", "uia.ps1")
	for _, automationId := range []string{idConnect, idAlert, idUpgrade, idKillSwitch, idNavConnect, idNavSettings,
		idPasswordUser, idPasswordNext, idPasswordInput, idPasswordSubmit} {
		if !strings.Contains(app, `"`+automationId+`"`) {
			t.Errorf("the app does not set %s", automationId)
		}
		if !strings.Contains(guest, automationId) && !strings.Contains(uia, `"`+automationId+`"`) {
			t.Errorf("neither guest script uses %s", automationId)
		}
	}
	if !strings.Contains(guest, `"`+strings.Join([]string{idConnect, idAlert, idUpgrade}, "|")+`"`) {
		t.Error("guest.ps1 observe does not ask for the connect button, alert and upgrade")
	}
	marker := regexp.MustCompile(`\$NoticeMarker = "([^"]+)"`).FindStringSubmatch(guest)
	if marker == nil || !strings.Contains(read("app", "src", "App", "AppController.cpp"), `LogInfo("`+marker[1]+`");`) {
		t.Error("guest.ps1 counts a notice line the app does not write")
	}
}

// ---- driver verbs over a fake VM ----

type fakeVm struct {
	t        *testing.T
	booted   int
	alive    bool
	commands []string
	copies   []string
	dumps    int
	shutdown int
	// guest verb (with -Op) -> result JSON, or an error
	reply func(verb, op, arg string) (string, error)
}

var guestCommandPattern = regexp.MustCompile(`-Verb (\S+)(?: -Op "([^"]*)")?(?: -Arg "([^"]*)")?`)

func (self *fakeVm) Boot(ctx context.Context) (*vmState, error) {
	self.booted++
	self.alive = true
	return &vmState{QemuPid: 4242, RunDir: "/tmp/tmp.fake", MonSock: "/tmp/tmp.fake/mon.sock", SshPort: "2222"}, nil
}
func (self *fakeVm) Alive(st *vmState) bool { return self.alive && st != nil && st.QemuPid == 4242 }
func (self *fakeVm) WaitSsh(ctx context.Context, st *vmState, tries int) error {
	return nil
}
func (self *fakeVm) Prepare(ctx context.Context, st *vmState) error { return nil }
func (self *fakeVm) Ssh(ctx context.Context, st *vmState, deadlineSeconds int, command string) (string, error) {
	self.commands = append(self.commands, command)
	m := guestCommandPattern.FindStringSubmatch(command)
	if m == nil {
		return "", nil
	}
	out, err := self.reply(m[1], m[2], m[3])
	if err != nil {
		return "", err
	}
	return "diagnostic line\nIBRESULT " + out + "\n", nil
}
func (self *fakeVm) CopyTo(ctx context.Context, st *vmState, local, remote string) error {
	self.copies = append(self.copies, remote)
	if strings.HasSuffix(remote, "credentials.json") {
		b, err := os.ReadFile(local)
		if err != nil {
			return err
		}
		info, _ := os.Stat(local)
		if info.Mode().Perm() != 0600 || !bytes.Contains(b, []byte(`"password":"pw"`)) {
			self.t.Errorf("credentials copy: mode %v", info.Mode().Perm())
		}
	}
	return nil
}
func (self *fakeVm) CopyFrom(ctx context.Context, st *vmState, remote, local string) error {
	self.copies = append(self.copies, "<-"+remote)
	return nil
}
func (self *fakeVm) Screendump(ctx context.Context, st *vmState, path string) error {
	self.dumps++
	return nil
}
func (self *fakeVm) Shutdown(ctx context.Context, st *vmState) error {
	self.shutdown++
	self.alive = false
	return nil
}

func (self *fakeVm) verbs() []string {
	var verbs []string
	for _, c := range self.commands {
		if m := guestCommandPattern.FindStringSubmatch(c); m != nil {
			v := m[1]
			if m[2] != "" {
				v += ":" + m[2]
			}
			if m[3] != "" && m[1] != "install" {
				v += ":" + m[3]
			}
			verbs = append(verbs, v)
		}
	}
	return verbs
}

type fakeClock struct{ now time.Time }

func (self *fakeClock) sleep(ctx context.Context, d time.Duration) error {
	self.now = self.now.Add(d)
	return nil
}

func newTestDriver(t *testing.T, v *fakeVm) (*driver, string) {
	t.Helper()
	dir := t.TempDir()
	msi := filepath.Join(dir, "URnetwork-0.0.0-0-arm64.msi")
	if err := os.WriteFile(msi, []byte("msi"), 0600); err != nil {
		t.Fatal(err)
	}
	state := filepath.Join(dir, "state")
	if err := os.MkdirAll(state, 0700); err != nil {
		t.Fatal(err)
	}
	privateDir := filepath.Join(dir, "private")
	clock := &fakeClock{now: time.Unix(0, 0)}
	return &driver{
		vm:        v,
		env:       environment{root: dir, stateDir: state, credentials: filepath.Join(dir, "creds"), msi: msi},
		scriptDir: dir,
		acceptLib: filepath.Join(dir, "run-windows-lib.ps1"),
		sleep:     clock.sleep,
		now:       func() time.Time { return clock.now },
		readCreds: func(string) (credentials, error) {
			return credentials{Email: "ib@example.com", Password: "pw", PasswordSendKeys: "pw"}, nil
		},
		privateTemp: func() (string, error) { return privateDir, os.MkdirAll(privateDir, 0700) },
	}, privateDir
}

const disconnectedFacts = `{"window":true,"elements":{"acceptance.connect":{"present":true,"offscreen":false,"enabled":true,"name":"Connect"}}}`

// A guest that boots, reboots into the autologon session and signs in.
func setupReply(connectedAfterLogin bool) func(verb, op, arg string) (string, error) {
	rebooted := false
	bootTimeCalls := 0
	return func(verb, op, arg string) (string, error) {
		switch verb {
		case "enable-autologon":
			return `{"boot_time":"t0"}`, nil
		case "reboot":
			rebooted = true
			return `{}`, nil
		case "boot-time":
			bootTimeCalls++
			if bootTimeCalls == 1 {
				// still going down
				return "", errors.New("ssh: connection refused")
			}
			if rebooted && bootTimeCalls >= 3 {
				return `{"boot_time":"t1"}`, nil
			}
			return `{"boot_time":"t0"}`, nil
		case "session":
			return `{"interactive":true}`, nil
		case "install":
			return `{"app":"C:\\Program Files\\URnetwork\\URnetwork.exe"}`, nil
		case "interactive":
			switch op {
			case "launch":
				return `{"window":true}`, nil
			case "login":
				return `{"signed_in":true,"detail":""}`, nil
			case "kill-switch":
				return fmt.Sprintf(`{"found":true,"on":%v}`, arg == "on"), nil
			}
		case "notices":
			return `{"count":4}`, nil
		case "observe":
			if connectedAfterLogin {
				return `{"uia":{"window":true,"elements":{"acceptance.connect":{"present":true,"enabled":true,"name":"Disconnect"}}},"routes":["0.0.0.0/1"],"notices":4}`, nil
			}
			return `{"uia":` + disconnectedFacts + `,"routes":[],"notices":4}`, nil
		case "cleanup-private", "collect":
			return `{"files":[]}`, nil
		}
		return "", fmt.Errorf("unexpected guest verb %s %s", verb, op)
	}
}

func TestSetup(t *testing.T) {
	v := &fakeVm{t: t}
	v.reply = setupReply(false)
	d, privateDir := newTestDriver(t, v)
	out, err := d.run(context.Background(), []string{"setup"})
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := json.Marshal(out); string(b) != `{"kill_switch_supported":true}` {
		t.Fatalf("setup output %s", b)
	}
	want := []string{"enable-autologon", "reboot", "boot-time", "boot-time", "boot-time", "session", "install",
		"interactive:launch", "interactive:login", "cleanup-private", "interactive:kill-switch:off", "notices", "observe"}
	if got := v.verbs(); strings.Join(got, ",") != strings.Join(want, ",") {
		t.Fatalf("guest verbs\n got %v\nwant %v", got, want)
	}
	st, err := loadState(d.env.stateDir)
	if err != nil || st == nil || st.Notices != 4 || st.QemuPid != 4242 {
		t.Fatalf("state %+v, %v", st, err)
	}
	if _, err := os.Stat(privateDir); !errors.Is(err, os.ErrNotExist) {
		t.Fatal("the local private credentials copy was not removed")
	}
	if v.booted != 1 || v.shutdown != 0 {
		t.Fatalf("booted %d shutdown %d", v.booted, v.shutdown)
	}
}

func TestSetupRefusesAConnectedApp(t *testing.T) {
	v := &fakeVm{t: t}
	v.reply = setupReply(true)
	d, _ := newTestDriver(t, v)
	if _, err := d.run(context.Background(), []string{"setup"}); err == nil || !strings.Contains(err.Error(), "connected after sign-in") {
		t.Fatalf("got %v", err)
	}
	// the VM stays recorded so the runner's teardown stops it
	if st, _ := loadState(d.env.stateDir); st == nil {
		t.Fatal("state was not kept for teardown")
	}
}

func TestSetupRemovesCredentialsWhenSignInFails(t *testing.T) {
	v := &fakeVm{t: t}
	base := setupReply(false)
	v.reply = func(verb, op, arg string) (string, error) {
		if op == "login" {
			return `{"signed_in":false,"detail":"the password step did not open (account discovery)"}`, nil
		}
		return base(verb, op, arg)
	}
	d, _ := newTestDriver(t, v)
	_, err := d.run(context.Background(), []string{"setup"})
	if err == nil || !strings.Contains(err.Error(), "account discovery") {
		t.Fatalf("got %v", err)
	}
	if verbs := v.verbs(); verbs[len(verbs)-1] != "cleanup-private" {
		t.Fatalf("credentials not cleaned up: %v", verbs)
	}
}

func TestSetupNeedsTheBuiltMsi(t *testing.T) {
	v := &fakeVm{t: t}
	d, _ := newTestDriver(t, v)
	d.env.msi = filepath.Join(t.TempDir(), "missing.msi")
	if _, err := d.run(context.Background(), []string{"setup"}); err == nil || v.booted != 0 {
		t.Fatalf("got %v, booted %d", err, v.booted)
	}
}

func startedDriver(t *testing.T, reply func(verb, op, arg string) (string, error)) (*driver, *fakeVm) {
	t.Helper()
	v := &fakeVm{t: t, alive: true, reply: reply}
	d, _ := newTestDriver(t, v)
	if err := saveState(d.env.stateDir, &vmState{QemuPid: 4242, RunDir: "/tmp/tmp.fake", Notices: 2}); err != nil {
		t.Fatal(err)
	}
	return d, v
}

func TestObserveCountsSinceSetupAndKeepsOneScreendump(t *testing.T) {
	d, v := startedDriver(t, func(verb, op, arg string) (string, error) {
		b, _ := json.Marshal(heldGuest())
		return string(b), nil
	})
	for i := 0; i < 3; i++ {
		out, err := d.run(context.Background(), []string{"observe"})
		if err != nil {
			t.Fatal(err)
		}
		b, _ := json.Marshal(out)
		want := `{"connect_requested":true,"connected":true,"insufficient_balance_alert":true,"disconnect_visible":true,"upgrade_visible":true,"insufficient_balance_notifications":1}`
		if string(b) != want {
			t.Fatalf("observe %s", b)
		}
	}
	if v.dumps != 1 {
		t.Fatalf("screendumps %d, want 1", v.dumps)
	}
}

func TestPressDisconnectOnlyPressesDisconnect(t *testing.T) {
	d, v := startedDriver(t, func(verb, op, arg string) (string, error) {
		if op != "invoke" {
			return "", errors.New("unexpected")
		}
		target, label, _ := strings.Cut(arg, "|")
		if target != idConnect {
			return "", errors.New("wrong control")
		}
		// the button says Connect: uia.ps1 does not press it
		return fmt.Sprintf(`{"present":true,"invoked":%v,"name":"Connect"}`, label == labelConnect), nil
	})
	if _, err := d.run(context.Background(), []string{"press-disconnect"}); err == nil || !strings.Contains(err.Error(), `"Connect"`) {
		t.Fatalf("got %v", err)
	}
	if _, err := d.run(context.Background(), []string{"connect"}); err != nil {
		t.Fatal(err)
	}
	if verbs := v.verbs(); verbs[0] != "interactive:invoke:acceptance.connect|Disconnect" || verbs[1] != "interactive:invoke:acceptance.connect|Connect" {
		t.Fatalf("verbs %v", verbs)
	}
}

func TestEgressVerbs(t *testing.T) {
	held := `{"error":"https://checkip.amazonaws.com/=curl 28"}`
	d, _ := startedDriver(t, func(verb, op, arg string) (string, error) { return held, nil })
	out, err := d.run(context.Background(), []string{"egress"})
	if err != nil {
		t.Fatal(err)
	}
	if b, _ := json.Marshal(out); string(b) != held {
		t.Fatalf("egress %s", b)
	}
	// the direct address must exist; a held probe there is a setup failure
	if _, err := d.run(context.Background(), []string{"direct-egress"}); err == nil {
		t.Fatal("direct-egress accepted no address")
	}
}

func TestGuestFailureIsNotAHeldProbe(t *testing.T) {
	d, _ := startedDriver(t, func(verb, op, arg string) (string, error) {
		return "", errors.New("exit status 255: ssh: connect to host 127.0.0.1 port 2222: Connection refused")
	})
	if _, err := d.run(context.Background(), []string{"egress"}); err == nil {
		t.Fatal("an unreachable guest was reported as a held probe")
	}
}

func TestKillSwitch(t *testing.T) {
	d, _ := startedDriver(t, func(verb, op, arg string) (string, error) {
		return `{"found":true,"on":false}`, nil
	})
	if _, err := d.run(context.Background(), []string{"kill-switch", "off"}); err != nil {
		t.Fatal(err)
	}
	if _, err := d.run(context.Background(), []string{"kill-switch", "on"}); err == nil {
		t.Fatal("a toggle that stayed off was accepted")
	}
	if _, err := d.run(context.Background(), []string{"kill-switch", "maybe"}); err == nil {
		t.Fatal("bad argument accepted")
	}
}

func TestVerbsNeedARunningVm(t *testing.T) {
	v := &fakeVm{t: t}
	d, _ := newTestDriver(t, v)
	if _, err := d.run(context.Background(), []string{"observe"}); err == nil || !strings.Contains(err.Error(), "setup did not complete") {
		t.Fatalf("got %v", err)
	}
	if len(v.commands) != 0 {
		t.Fatal("contacted a guest that is not running")
	}
}

func TestTeardown(t *testing.T) {
	// nothing set up: nothing to do
	v := &fakeVm{t: t}
	d, _ := newTestDriver(t, v)
	if _, err := d.run(context.Background(), []string{"teardown"}); err != nil || v.shutdown != 0 {
		t.Fatalf("got %v, shutdown %d", err, v.shutdown)
	}

	d, v = startedDriver(t, func(verb, op, arg string) (string, error) {
		if verb == "collect" {
			return `{"files":["urnetwork-app.log","urnetworkd.log","../escape"]}`, nil
		}
		return `{}`, nil
	})
	if _, err := d.run(context.Background(), []string{"teardown"}); err != nil {
		t.Fatal(err)
	}
	if strings.Join(v.verbs(), ",") != "collect,cleanup-private" || v.shutdown != 1 {
		t.Fatalf("verbs %v shutdown %d", v.verbs(), v.shutdown)
	}
	if strings.Join(v.copies, ",") != "<-C:/acceptance-ib/out/urnetwork-app.log,<-C:/acceptance-ib/out/urnetworkd.log" {
		t.Fatalf("copies %v", v.copies)
	}
	if st, _ := loadState(d.env.stateDir); st != nil {
		t.Fatal("state remained after teardown")
	}

	// a dead VM is not contacted, but its run dir is still cleaned up
	d, v = startedDriver(t, nil)
	v.alive = false
	if _, err := d.run(context.Background(), []string{"teardown"}); err != nil || len(v.commands) != 0 || v.shutdown != 1 {
		t.Fatalf("got %v, commands %v, shutdown %d", err, v.commands, v.shutdown)
	}
}

func TestMainCodeOutputAndRedaction(t *testing.T) {
	dir := t.TempDir()
	creds := filepath.Join(dir, "creds")
	if err := os.WriteFile(creds, []byte("email: ib@example.com\npassword: hunter2\n"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("URNETWORK_ROOT", dir)
	t.Setenv("URNETWORK_INSUFFICIENT_BALANCE_STATE", filepath.Join(dir, "state"))
	t.Setenv("URNETWORK_INSUFFICIENT_BALANCE_CREDENTIALS", creds)
	t.Setenv("URNETWORK_IB_WINDOWS_DIR", "")
	t.Setenv("EXTERNAL_WARP_VERSION", "")
	t.Setenv("UR_ACCEPT_WINDOWS_OUT", "")

	var reply func(verb, op, arg string) (string, error)
	newFake := func(e environment) *driver {
		v := &fakeVm{t: t, alive: true, reply: reply}
		d, _ := newTestDriver(t, v)
		d.env = e
		return d
	}
	if err := os.MkdirAll(filepath.Join(dir, "state"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := saveState(filepath.Join(dir, "state"), &vmState{QemuPid: 4242, RunDir: "/tmp/tmp.fake"}); err != nil {
		t.Fatal(err)
	}

	reply = func(verb, op, arg string) (string, error) { return `{"ip":"203.0.113.7"}`, nil }
	var stdout, stderr bytes.Buffer
	if code := mainCode(context.Background(), []string{"egress"}, &stdout, &stderr, newFake); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	if stdout.String() != "{\"ip\":\"203.0.113.7\"}\n" {
		t.Fatalf("stdout %q", stdout.String())
	}

	reply = func(verb, op, arg string) (string, error) {
		return "", errors.New("guest said hunter2 for\nib@example.com")
	}
	stdout.Reset()
	stderr.Reset()
	if code := mainCode(context.Background(), []string{"egress"}, &stdout, &stderr, newFake); code != 1 {
		t.Fatalf("exit %d", code)
	}
	if stdout.Len() != 0 || strings.Count(stderr.String(), "\n") != 1 ||
		strings.Contains(stderr.String(), "hunter2") || strings.Contains(stderr.String(), "ib@example.com") {
		t.Fatalf("stdout %q stderr %q", stdout.String(), stderr.String())
	}

	if e, err := loadEnvironment(); err != nil || e.msi != filepath.Join(dir, "windows", "out", "acceptance", "URnetwork-0.0.0-0-arm64.msi") {
		t.Fatalf("msi %q, %v", e.msi, err)
	}
}
