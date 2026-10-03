// SPDX-License-Identifier: MPL-2.0

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// The isolated VM as test-main.sh uses it (vm.go implements it over
// build/all/windows/lib.sh; tests use a fake).
type vm interface {
	// Boots a throwaway overlay of the base image in the background.
	Boot(ctx context.Context) (*vmState, error)
	// Whether st's QEMU process is still running.
	Alive(st *vmState) bool
	WaitSsh(ctx context.Context, st *vmState, tries int) error
	// Re-applies the hermetic guest policy (win_prepare_hermetic_guest).
	Prepare(ctx context.Context, st *vmState) error
	// Runs one guest command under a hard deadline and returns its stdout.
	Ssh(ctx context.Context, st *vmState, deadlineSeconds int, command string) (string, error)
	CopyTo(ctx context.Context, st *vmState, local, remote string) error
	CopyFrom(ctx context.Context, st *vmState, remote, local string) error
	// Saves the guest screen (QEMU monitor screendump).
	Screendump(ctx context.Context, st *vmState, path string) error
	// Graceful shutdown, then kill; removes the run dir.
	Shutdown(ctx context.Context, st *vmState) error
}

type driver struct {
	vm          vm
	env         environment
	scriptDir   string // guest.ps1, uia.ps1
	acceptLib   string // build/all/acceptance/run-windows-lib.ps1
	sleep       func(context.Context, time.Duration) error
	now         func() time.Time
	readCreds   func(string) (credentials, error)
	privateTemp func() (string, error)
}

// Guest ssh deadlines in seconds.
const (
	quickDeadline       = 120
	interactiveDeadline = 240
	installDeadline     = 600
)

func (self *driver) run(ctx context.Context, args []string) (any, error) {
	verb := args[0]
	if verb == "kill-switch" {
		if len(args) != 2 || (args[1] != "on" && args[1] != "off") {
			return nil, errors.New("usage: kill-switch on|off")
		}
		return self.killSwitch(ctx, args[1] == "on")
	}
	if len(args) != 1 {
		return nil, fmt.Errorf("%s takes no arguments", verb)
	}
	switch verb {
	case "setup":
		return self.setup(ctx)
	case "teardown":
		return struct{}{}, self.teardown(ctx)
	}
	st, err := self.liveState()
	if err != nil {
		return nil, err
	}
	switch verb {
	case "direct-egress":
		e, err := self.egress(ctx, st)
		if err != nil {
			return nil, err
		}
		if e.Ip == "" {
			return nil, fmt.Errorf("no direct address: %s", e.Error)
		}
		return e, nil
	case "egress":
		return self.egress(ctx, st)
	case "connect":
		return struct{}{}, self.invoke(ctx, st, idConnect, labelConnect)
	case "press-disconnect":
		return struct{}{}, self.invoke(ctx, st, idConnect, labelDisconnect)
	case "observe":
		return self.observe(ctx, st)
	case "traffic":
		// a held or failed transfer is still traffic offered to the tunnel
		_, err := self.guest(ctx, st, quickDeadline, "traffic")
		return struct{}{}, err
	}
	return nil, fmt.Errorf("unknown verb %q", verb)
}

func (self *driver) liveState() (*vmState, error) {
	st, err := loadState(self.env.stateDir)
	if err != nil {
		return nil, err
	}
	if st == nil || !self.vm.Alive(st) {
		return nil, errors.New("the Windows VM is not running; setup did not complete")
	}
	return st, nil
}

// Runs one guest.ps1 verb and returns its result object.
func (self *driver) guest(ctx context.Context, st *vmState, deadlineSeconds int, verb string, params ...string) (json.RawMessage, error) {
	command, err := guestCommand(verb, params...)
	if err != nil {
		return nil, err
	}
	stdout, err := self.vm.Ssh(ctx, st, deadlineSeconds, command)
	if err != nil {
		return nil, fmt.Errorf("guest %s: %w", verb, err)
	}
	result, err := parseGuestResult(stdout)
	if err != nil {
		return nil, fmt.Errorf("guest %s: %w", verb, err)
	}
	return result, nil
}

func (self *driver) guestInto(ctx context.Context, st *vmState, deadlineSeconds int, out any, verb string, params ...string) error {
	result, err := self.guest(ctx, st, deadlineSeconds, verb, params...)
	if err != nil {
		return err
	}
	if err := json.Unmarshal(result, out); err != nil {
		return fmt.Errorf("guest %s: unexpected result", verb)
	}
	return nil
}

func (self *driver) setup(ctx context.Context) (any, error) {
	creds, err := self.readCreds(self.env.credentials)
	if err != nil {
		return nil, err
	}
	msiHash, err := fileSha256(self.env.msi)
	if err != nil {
		return nil, fmt.Errorf("the locally built ARM64 MSI is missing (windows/test-main.sh builds it): %w", errors.Unwrap(err))
	}
	// a VM left by an interrupted attempt is never reused
	if err := self.teardown(ctx); err != nil {
		return nil, err
	}

	st, err := self.vm.Boot(ctx)
	if err != nil {
		return nil, fmt.Errorf("boot the Windows VM: %w", err)
	}
	// saved before anything else so teardown can always stop it
	if err := saveState(self.env.stateDir, st); err != nil {
		return nil, err
	}
	if err := self.vm.WaitSsh(ctx, st, 36); err != nil {
		return nil, fmt.Errorf("the Windows VM did not reach ssh: %w", err)
	}
	if err := self.vm.Prepare(ctx, st); err != nil {
		return nil, fmt.Errorf("hermetic guest policy: %w", err)
	}
	if _, err := self.vm.Ssh(ctx, st, quickDeadline, fmt.Sprintf(`powershell -NoProfile -Command "New-Item -ItemType Directory -Force -Path '%s' | Out-Null"`, guestDir)); err != nil {
		return nil, fmt.Errorf("guest directory: %w", err)
	}
	for _, copy := range [][2]string{
		{filepath.Join(self.scriptDir, "guest.ps1"), guestDir + "/guest.ps1"},
		{filepath.Join(self.scriptDir, "uia.ps1"), guestDir + "/uia.ps1"},
		{self.acceptLib, guestDir + "/run-windows-lib.ps1"},
		{self.env.msi, guestDir + "/urnetwork.msi"},
	} {
		if err := self.vm.CopyTo(ctx, st, copy[0], copy[1]); err != nil {
			return nil, fmt.Errorf("copy %s to the guest: %w", filepath.Base(copy[0]), err)
		}
	}

	// UI Automation needs the interactive desktop: autologon, then reboot
	var boot struct {
		BootTime string `json:"boot_time"`
	}
	if err := self.guestInto(ctx, st, quickDeadline, &boot, "enable-autologon"); err != nil {
		return nil, err
	}
	if _, err := self.guest(ctx, st, quickDeadline, "reboot"); err != nil {
		return nil, err
	}
	if err := self.poll(ctx, 6*time.Minute, "the guest reboot", func() (bool, error) {
		var now struct {
			BootTime string `json:"boot_time"`
		}
		// the guest is unreachable while it restarts
		if err := self.guestInto(ctx, st, 20, &now, "boot-time"); err != nil {
			return false, nil
		}
		return now.BootTime != "" && now.BootTime != boot.BootTime, nil
	}); err != nil {
		return nil, err
	}
	if err := self.poll(ctx, 3*time.Minute, "the builder desktop session (autologon)", func() (bool, error) {
		var session struct {
			Interactive bool `json:"interactive"`
		}
		err := self.guestInto(ctx, st, quickDeadline, &session, "session")
		return err == nil && session.Interactive, err
	}); err != nil {
		return nil, err
	}

	if _, err := self.guest(ctx, st, installDeadline, "install", "Arg", msiHash); err != nil {
		return nil, err
	}
	var launch struct {
		Window bool `json:"window"`
	}
	if err := self.guestInto(ctx, st, interactiveDeadline, &launch, "interactive", "Op", "launch"); err != nil {
		return nil, err
	}
	if !launch.Window {
		return nil, errors.New("the URnetwork window did not open")
	}
	if err := self.signIn(ctx, st, creds); err != nil {
		return nil, err
	}

	var killSwitch struct {
		Found bool `json:"found"`
		On    bool `json:"on"`
	}
	// the user's default; the runner's kill-switch case turns it on itself
	if err := self.guestInto(ctx, st, interactiveDeadline, &killSwitch, "interactive", "Op", "kill-switch", "Arg", "off"); err != nil {
		return nil, err
	}
	if killSwitch.Found && killSwitch.On {
		return nil, errors.New("the kill switch did not turn off")
	}

	var notices struct {
		Count int `json:"count"`
	}
	if err := self.guestInto(ctx, st, quickDeadline, &notices, "notices"); err != nil {
		return nil, err
	}
	st.Notices = notices.Count
	if err := saveState(self.env.stateDir, st); err != nil {
		return nil, err
	}
	o, err := self.observe(ctx, st)
	if err != nil {
		return nil, err
	}
	if o.ConnectRequested || o.Connected {
		return nil, errors.New("the app is connected after sign-in; setup must leave it disconnected")
	}
	return struct {
		KillSwitch bool `json:"kill_switch_supported"`
	}{killSwitch.Found}, nil
}

// Password sign-in through the GUI. The values travel in a private file that
// the guest deletes as soon as the step ends.
func (self *driver) signIn(ctx context.Context, st *vmState, creds credentials) (returnErr error) {
	dir, err := self.privateTemp()
	if err != nil {
		return err
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "credentials.json")
	b, err := json.Marshal(creds)
	if err != nil {
		return err
	}
	if err := os.WriteFile(path, b, 0600); err != nil {
		return err
	}
	defer func() {
		if _, err := self.guest(context.WithoutCancel(ctx), st, quickDeadline, "cleanup-private"); err != nil {
			returnErr = errors.Join(returnErr, err)
		}
	}()
	if err := self.vm.CopyTo(ctx, st, path, guestDir+"/credentials.json"); err != nil {
		return fmt.Errorf("copy credentials to the guest: %w", err)
	}
	var login struct {
		SignedIn bool   `json:"signed_in"`
		Detail   string `json:"detail"`
	}
	if err := self.guestInto(ctx, st, interactiveDeadline, &login, "interactive", "Op", "login"); err != nil {
		return err
	}
	if !login.SignedIn {
		return fmt.Errorf("password sign-in did not reach the connect page: %s", bounded(login.Detail, 160))
	}
	return nil
}

func (self *driver) observe(ctx context.Context, st *vmState) (observation, error) {
	var g guestObservation
	if err := self.guestInto(ctx, st, interactiveDeadline, &g, "observe"); err != nil {
		return observation{}, err
	}
	o, err := observationFromGuest(g, st.Notices)
	if err != nil {
		return observation{}, err
	}
	if o.Alert && !st.Evidence {
		// the screen when the state is first seen: banner, buttons and any
		// balloon still on screen (best effort evidence)
		if err := self.vm.Screendump(ctx, st, filepath.Join(self.env.stateDir, "insufficient-balance-alert.ppm")); err == nil {
			st.Evidence = true
			_ = saveState(self.env.stateDir, st)
		}
	}
	return o, nil
}

func (self *driver) egress(ctx context.Context, st *vmState) (egressResult, error) {
	var g guestEgress
	if err := self.guestInto(ctx, st, quickDeadline, &g, "egress"); err != nil {
		return egressResult{}, err
	}
	return egressFromGuest(g), nil
}

// Invokes the explicit connect button only when it says expectedLabel, so a
// press can never be the opposite action.
func (self *driver) invoke(ctx context.Context, st *vmState, automationId, expectedLabel string) error {
	var r struct {
		Invoked bool   `json:"invoked"`
		Name    string `json:"name"`
		Present bool   `json:"present"`
	}
	if err := self.guestInto(ctx, st, interactiveDeadline, &r, "interactive", "Op", "invoke", "Arg", automationId+"|"+expectedLabel); err != nil {
		return err
	}
	switch {
	case !r.Present:
		return fmt.Errorf("%s is not in the window", automationId)
	case !r.Invoked:
		return fmt.Errorf("%s says %q, not %q", automationId, bounded(r.Name, 40), expectedLabel)
	}
	return nil
}

func (self *driver) killSwitch(ctx context.Context, on bool) (any, error) {
	st, err := self.liveState()
	if err != nil {
		return nil, err
	}
	state := "off"
	if on {
		state = "on"
	}
	var r struct {
		Found bool `json:"found"`
		On    bool `json:"on"`
	}
	if err := self.guestInto(ctx, st, interactiveDeadline, &r, "interactive", "Op", "kill-switch", "Arg", state); err != nil {
		return nil, err
	}
	switch {
	case !r.Found:
		return nil, errors.New("the kill switch toggle is not in Settings")
	case r.On != on:
		return nil, fmt.Errorf("the kill switch did not turn %s", state)
	}
	return struct{}{}, nil
}

// Collects the app and service logs, removes private files and stops the VM.
// Safe to call at any point, including before or after a partial setup.
func (self *driver) teardown(ctx context.Context) error {
	st, err := loadState(self.env.stateDir)
	if err != nil || st == nil {
		return err
	}
	var errs []error
	if self.vm.Alive(st) {
		collectCtx, cancel := context.WithTimeout(ctx, 90*time.Second)
		var collected struct {
			Files []string `json:"files"`
		}
		if err := self.guestInto(collectCtx, st, 60, &collected, "collect"); err == nil {
			logDir := filepath.Join(self.env.stateDir, "logs")
			if err := os.MkdirAll(logDir, 0700); err == nil {
				for _, name := range collected.Files {
					if !guestValuePattern.MatchString(name) {
						continue
					}
					_ = self.vm.CopyFrom(collectCtx, st, guestDir+"/out/"+name, filepath.Join(logDir, name))
				}
			}
		}
		if _, err := self.guest(collectCtx, st, 30, "cleanup-private"); err != nil {
			errs = append(errs, err)
		}
		cancel()
	}
	if err := self.vm.Shutdown(ctx, st); err != nil {
		errs = append(errs, fmt.Errorf("stop the Windows VM: %w", err))
	} else if err := removeState(self.env.stateDir); err != nil {
		errs = append(errs, err)
	}
	return errors.Join(errs...)
}

// Polls fn every 5 s until done, an error, or the limit.
func (self *driver) poll(ctx context.Context, limit time.Duration, what string, fn func() (bool, error)) error {
	deadline := self.now().Add(limit)
	for {
		done, err := fn()
		if err != nil {
			return fmt.Errorf("%s: %w", what, err)
		}
		if done {
			return nil
		}
		if !self.now().Before(deadline) {
			return fmt.Errorf("%s: not reached within %s", what, limit)
		}
		if err := self.sleep(ctx, 5*time.Second); err != nil {
			return fmt.Errorf("%s: %w", what, err)
		}
	}
}

func fileSha256(path string) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer file.Close()
	h := sha256.New()
	if _, err := io.Copy(h, file); err != nil {
		return "", err
	}
	return strings.ToUpper(hex.EncodeToString(h.Sum(nil))), nil
}
