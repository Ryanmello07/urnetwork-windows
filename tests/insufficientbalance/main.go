// SPDX-License-Identifier: MPL-2.0

// Windows driver for MAIN's insufficient-balance case. The protocol, timing
// and assertions belong to tests/runner/balance/main.go and
// tests/runner/RUN-MAIN.md (Driver protocol); this program only owns the
// Windows VM and reports what the app shows.
//
// Each verb is a separate process, so everything that must survive between
// verbs (the QEMU pid, its run dir and monitor socket, the notice baseline)
// lives in state.json in URNETWORK_INSUFFICIENT_BALANCE_STATE. The VM is the
// repository's isolated Windows 11 ARM64 QEMU guest, booted, reached and shut
// down through build/all/windows/lib.sh exactly as test-main.sh does, with
// the MSI that test-main.sh built. The GUI is driven with UI Automation by
// acceptance.* automation ids. UI Automation only sees the interactive
// desktop, so setup turns on autologon for the VM's builder account and
// reboots, and every UI operation runs as a scheduled task in that session
// (guest.ps1 Invoke-Interactive, uia.ps1). Egress probes and traffic run in
// the ssh session, a non-app process. The tray balloon cannot be read back
// from the shell, so notices are counted from the app log line the notice
// sink writes (AppController::ReactToBalance).
//
// Credentials arrive only as a private file path; the values are copied to
// the guest for the sign-in step, deleted there right after, and never
// printed.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// Guest directory for the driver's scripts, MSI and exchange files.
const guestDir = "C:/acceptance-ib"

// UI Automation ids set by the app (tests/acceptance_automation_ids_test.go).
const (
	idConnect        = "acceptance.connect"
	idAlert          = "acceptance.insufficient-balance.alert"
	idUpgrade        = "acceptance.insufficient-balance.upgrade"
	idKillSwitch     = "acceptance.settings.kill-switch"
	idNavConnect     = "acceptance.nav.connect"
	idNavSettings    = "acceptance.nav.settings"
	idPasswordUser   = "acceptance.password.user"
	idPasswordNext   = "acceptance.password.next"
	idPasswordInput  = "acceptance.password.input"
	idPasswordSubmit = "acceptance.password.submit"
)

// The explicit connect button's en labels (Strings/en/Resources.resw keys
// connect, disconnect, retry). The VM runs en-US.
const (
	labelConnect    = "Connect"
	labelDisconnect = "Disconnect"
	labelRetry      = "Retry"
)

// Default local MSI version, as in test-main.sh.
const defaultVersion = "0.0.0-0"

// ---- protocol output ----

type observation struct {
	ConnectRequested bool `json:"connect_requested"`
	Connected        bool `json:"connected"`
	Alert            bool `json:"insufficient_balance_alert"`
	DisconnectButton bool `json:"disconnect_visible"`
	UpgradeButton    bool `json:"upgrade_visible"`
	Notifications    int  `json:"insufficient_balance_notifications"`
}

type egressResult struct {
	Ip    string `json:"ip,omitempty"`
	Error string `json:"error,omitempty"`
}

// ---- guest replies ----

// Facts uia.ps1 reports for one automation id.
type elementFacts struct {
	Present   bool   `json:"present"`
	Offscreen bool   `json:"offscreen"`
	Enabled   bool   `json:"enabled"`
	Name      string `json:"name"`
}

type uiaFacts struct {
	Window            bool                    `json:"window"`
	AutomationIdFacts map[string]elementFacts `json:"elements"`
}

type guestObservation struct {
	Uia      uiaFacts `json:"uia"`
	Prefixes []string `json:"routes"`
	Notices  int      `json:"notices"`
}

type guestEgress struct {
	Ip    string `json:"ip"`
	Error string `json:"error"`
}

// Capture routes the service installs on the URnetwork adapter while a
// session is up; any of them means the tunnel holds the machine's traffic.
var capturePrefixes = map[string]bool{
	"0.0.0.0/0":   true,
	"0.0.0.0/1":   true,
	"128.0.0.0/1": true,
}

// Maps one guest observation to the protocol. Notices count posts since
// setup's baseline.
func observationFromGuest(g guestObservation, noticeBaseline int) (observation, error) {
	if !g.Uia.Window {
		return observation{}, errors.New("the URnetwork window is not on the interactive desktop")
	}
	connect := g.Uia.AutomationIdFacts[idConnect]
	if !connect.Present {
		return observation{}, errors.New("the connect button is not in the window (signed out or another page)")
	}
	if g.Notices < noticeBaseline {
		return observation{}, fmt.Errorf("the app log lost notice lines (%d < baseline %d)", g.Notices, noticeBaseline)
	}
	visible := func(f elementFacts) bool { return f.Present && !f.Offscreen }
	connected := false
	for _, prefix := range g.Prefixes {
		connected = connected || capturePrefixes[prefix]
	}
	alert := g.Uia.AutomationIdFacts[idAlert]
	upgrade := g.Uia.AutomationIdFacts[idUpgrade]
	return observation{
		// Retry is a failed session that still holds its destination
		ConnectRequested: connect.Name == labelDisconnect || connect.Name == labelRetry,
		Connected:        connected,
		Alert:            visible(alert),
		DisconnectButton: visible(connect) && connect.Enabled && connect.Name == labelDisconnect,
		UpgradeButton:    visible(upgrade) && upgrade.Enabled,
		Notifications:    g.Notices - noticeBaseline,
	}, nil
}

// A probe that returns no valid address is held (or failed); the runner
// classifies it against the direct address.
func egressFromGuest(g guestEgress) egressResult {
	ip := strings.TrimSpace(g.Ip)
	if ip != "" && net.ParseIP(ip) != nil {
		return egressResult{Ip: ip}
	}
	if ip != "" {
		return egressResult{Error: "invalid public address response"}
	}
	if g.Error == "" {
		return egressResult{Error: "no response"}
	}
	return egressResult{Error: bounded(g.Error, 160)}
}

// ---- guest command line ----

// Only these characters reach the guest's command line, so no value can
// break out of its quoting.
var guestValuePattern = regexp.MustCompile(`^[A-Za-z0-9.:|_-]+$`)

// One guest.ps1 invocation. params are name/value pairs.
func guestCommand(verb string, params ...string) (string, error) {
	if len(params)%2 != 0 {
		return "", errors.New("guest parameters must be name/value pairs")
	}
	if !guestValuePattern.MatchString(verb) {
		return "", fmt.Errorf("unsafe guest verb %q", verb)
	}
	var b strings.Builder
	fmt.Fprintf(&b, "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File %s/guest.ps1 -Verb %s", guestDir, verb)
	for i := 0; i < len(params); i += 2 {
		if !guestValuePattern.MatchString(params[i]) || !guestValuePattern.MatchString(params[i+1]) {
			return "", fmt.Errorf("unsafe guest parameter for %s", verb)
		}
		fmt.Fprintf(&b, ` -%s "%s"`, params[i], params[i+1])
	}
	return b.String(), nil
}

const guestResultMarker = "IBRESULT "

// The guest prints one marker line with its JSON; anything else on stdout is
// diagnostics. A missing marker is a guest failure, never a held probe.
func parseGuestResult(stdout string) (json.RawMessage, error) {
	var result json.RawMessage
	for _, line := range strings.Split(stdout, "\n") {
		line = strings.TrimRight(line, "\r")
		if !strings.HasPrefix(line, guestResultMarker) {
			continue
		}
		if result != nil {
			return nil, errors.New("guest printed more than one result")
		}
		raw := json.RawMessage(strings.TrimSpace(strings.TrimPrefix(line, guestResultMarker)))
		if !json.Valid(raw) || len(raw) == 0 || raw[0] != '{' {
			return nil, errors.New("guest result is not one JSON object")
		}
		result = raw
	}
	if result == nil {
		return nil, errors.New("guest printed no result")
	}
	return result, nil
}

// ---- credentials ----

type credentials struct {
	Email    string `json:"email"`
	Password string `json:"password"`
	// the password for SendKeys, used only when the PasswordBox refuses
	// ValuePattern.SetValue
	PasswordSendKeys string `json:"password_sendkeys"`
}

// Reads the private flat file with exactly `email:` and `password:` (the
// runner's format). Values are never printed.
func readCredentials(path string) (credentials, error) {
	info, err := os.Stat(path)
	if err != nil {
		return credentials{}, fmt.Errorf("credentials: %w", errors.Unwrap(err))
	}
	if info.Mode().Perm()&0077 != 0 {
		return credentials{}, errors.New("credentials file must not be group/world readable")
	}
	file, err := os.Open(path)
	if err != nil {
		return credentials{}, errors.New("credentials file cannot be opened")
	}
	defer file.Close()
	return parseCredentials(file)
}

func parseCredentials(r io.Reader) (credentials, error) {
	keyValues := map[string]string{}
	s := bufio.NewScanner(io.LimitReader(r, 64<<10))
	for s.Scan() {
		line := strings.TrimSpace(s.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, value, ok := strings.Cut(line, ":")
		key = strings.TrimSpace(key)
		if !ok || (key != "email" && key != "password") || keyValues[key] != "" {
			return credentials{}, errors.New("credentials: only one email and one password key are allowed")
		}
		value = strings.TrimSpace(value)
		if 2 <= len(value) && (value[0] == '"' || value[0] == '\'') && value[len(value)-1] == value[0] {
			value = value[1 : len(value)-1]
		}
		keyValues[key] = value
	}
	if err := s.Err(); err != nil {
		return credentials{}, errors.New("credentials file cannot be read")
	}
	if keyValues["email"] == "" || keyValues["password"] == "" {
		return credentials{}, errors.New("credentials: email and password are required")
	}
	return credentials{
		Email:            keyValues["email"],
		Password:         keyValues["password"],
		PasswordSendKeys: sendKeysEscape(keyValues["password"]),
	}, nil
}

// System.Windows.Forms.SendKeys treats + ^ % ~ ( ) { } [ ] as syntax; each is
// sent literally inside braces.
func sendKeysEscape(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch r {
		case '+', '^', '%', '~', '(', ')', '{', '}', '[', ']':
			b.WriteString("{" + string(r) + "}")
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

// ---- output and errors ----

func bounded(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

// One stderr line with any secret removed.
func redactedLine(message string, secrets ...string) string {
	for _, secret := range secrets {
		if secret != "" {
			message = strings.ReplaceAll(message, secret, "[redacted]")
		}
	}
	message = strings.Join(strings.Fields(message), " ")
	return bounded(message, 300)
}

// ---- state ----

type vmState struct {
	QemuPid  int    `json:"qemu_pid"`
	RunDir   string `json:"run_dir"`
	MonSock  string `json:"mon_sock"`
	SshPort  string `json:"ssh_port"`
	Notices  int    `json:"notice_baseline"`
	Evidence bool   `json:"alert_screendump"`
}

func loadState(dir string) (*vmState, error) {
	b, err := os.ReadFile(filepath.Join(dir, "state.json"))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var st vmState
	if err := json.Unmarshal(b, &st); err != nil {
		return nil, errors.New("state.json is malformed")
	}
	return &st, nil
}

func saveState(dir string, st *vmState) error {
	b, err := json.Marshal(st)
	if err != nil {
		return err
	}
	path := filepath.Join(dir, "state.json")
	if err := os.WriteFile(path+".tmp", b, 0600); err != nil {
		return err
	}
	return os.Rename(path+".tmp", path)
}

func removeState(dir string) error {
	err := os.Remove(filepath.Join(dir, "state.json"))
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	return err
}

// ---- entry ----

type environment struct {
	root        string
	windowsDir  string
	stateDir    string
	credentials string
	msi         string
}

func loadEnvironment() (environment, error) {
	e := environment{
		root:        os.Getenv("URNETWORK_ROOT"),
		stateDir:    os.Getenv("URNETWORK_INSUFFICIENT_BALANCE_STATE"),
		credentials: os.Getenv("URNETWORK_INSUFFICIENT_BALANCE_CREDENTIALS"),
	}
	if !filepath.IsAbs(e.root) || !filepath.IsAbs(e.stateDir) {
		return e, errors.New("URNETWORK_ROOT and URNETWORK_INSUFFICIENT_BALANCE_STATE must be absolute")
	}
	e.windowsDir = os.Getenv("URNETWORK_IB_WINDOWS_DIR")
	if e.windowsDir == "" {
		e.windowsDir = filepath.Join(e.root, "windows")
	}
	version := os.Getenv("EXTERNAL_WARP_VERSION")
	if version == "" {
		version = defaultVersion
	}
	if !guestValuePattern.MatchString(version) {
		return e, errors.New("EXTERNAL_WARP_VERSION contains unsupported characters")
	}
	outDir := os.Getenv("UR_ACCEPT_WINDOWS_OUT")
	if outDir == "" {
		outDir = filepath.Join(e.windowsDir, "out", "acceptance")
	}
	// the MSI windows/test-main.sh built earlier in MAIN; a build does not
	// fit the protocol's 10 minute verb limit
	e.msi = filepath.Join(outDir, fmt.Sprintf("URnetwork-%s-arm64.msi", version))
	return e, nil
}

func main() {
	os.Exit(mainCode(context.Background(), os.Args[1:], os.Stdout, os.Stderr, newDriver))
}

func newDriver(e environment) *driver {
	return &driver{
		vm:          newLibVm(filepath.Join(e.root, "build", "all", "windows", "lib.sh"), filepath.Join(e.stateDir, "qemu.log")),
		env:         e,
		scriptDir:   filepath.Join(e.windowsDir, "tests", "insufficientbalance"),
		acceptLib:   filepath.Join(e.root, "build", "all", "acceptance", "run-windows-lib.ps1"),
		sleep:       sleepContext,
		now:         time.Now,
		readCreds:   readCredentials,
		privateTemp: func() (string, error) { return os.MkdirTemp("", "urnetwork-ib-windows-") },
	}
}

// Runs one verb: exactly one JSON object on stdout and exit 0, or one
// redacted stderr line and a nonzero exit.
func mainCode(ctx context.Context, args []string, stdout, stderr io.Writer, newDriver func(environment) *driver) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: test-insufficient-balance-driver setup|direct-egress|connect|observe|egress|traffic|press-disconnect|kill-switch on|off|teardown")
		return 2
	}
	e, err := loadEnvironment()
	if err != nil {
		fmt.Fprintln(stderr, redactedLine(err.Error()))
		return 2
	}
	if err := os.MkdirAll(e.stateDir, 0700); err != nil {
		fmt.Fprintln(stderr, redactedLine(err.Error()))
		return 1
	}
	out, err := newDriver(e).run(ctx, args)
	if err != nil {
		var secrets []string
		if c, cerr := readCredentials(e.credentials); cerr == nil {
			secrets = []string{c.Password, c.Email}
		}
		fmt.Fprintln(stderr, redactedLine(fmt.Sprintf("windows %s: %v", args[0], err), secrets...))
		return 1
	}
	b, err := json.Marshal(out)
	if err != nil {
		fmt.Fprintln(stderr, redactedLine(err.Error()))
		return 1
	}
	fmt.Fprintln(stdout, string(b))
	return 0
}

func sleepContext(ctx context.Context, d time.Duration) error {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-t.C:
		return nil
	}
}
