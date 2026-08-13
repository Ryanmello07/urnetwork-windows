# URnetwork for Windows — beta line

Native Windows 10 21H2+ / Windows 11 client for x64 and ARM64. A WinUI 3 app
drives a privileged Windows service that owns the VPN tunnel; both embed the
URnetwork SDK (the cgo C ABI + C++ wrapper from `sdk/cgo`).

**This is the beta fork.** It tracks `urnetwork/windows` and runs ahead of it.
Work lands here, gets a beta build, gets tested on real machines, and is then
PR'd upstream. If you want the stable client, use upstream.

Two differences matter before you read further:

- This branch builds against the **fork branches** of `connect` and `sdk`
  (`beta/algorithm-dpi`), not upstream main. Building it against upstream
  `connect`/`sdk` will fail to compile — the smart-routing and probe APIs it
  calls only exist on those branches until each lands upstream.
- Beta releases are published here, not upstream. Upstream carries the CI that
  proves it builds; releases come from our own build pipeline.

## What the beta has that upstream does not

- **Opens on launch.** The window comes up on start rather than starting
  minimized to the tray.
- **Motion system** (`UrMotion`) — page crossfades, a reveal-spring on window
  open, and a reduce-motion gate that honors the OS setting.
- **Onboarding** (`Onboarding`) — first-run tray balloon, banner focus, and a
  Connect tip.
- **Simple mode as a real mode.** Simple is structurally separate from
  Advanced rather than Advanced-with-things-hidden: it has its own front page,
  and the Advanced surfaces are absent rather than merely collapsed.
- **Smart routing** (`connect` `beta/algorithm-dpi`) — a light-tier flow
  classifier, per-exit telemetry, a reward tap feeding persisted provider
  priors, and scored placement. See below for what is and is not live.

## Architecture

```
URnetwork.exe (tray, per-user)          urnetworkd.exe (service, LocalSystem)
  WinUI 3 window + tray flyout            DeviceLocal + wintun packet pump
  SdkHost: DeviceRemote --------------->  DeviceLocal.SetRpcServer (mTLS ws)
  ServiceClient (named pipe) ----------->  ControlServer -> TunnelController
                                           NetworkConfig (routes/DNS/MTU)
                                           WfpPolicy (leak guards, kill switch)
                                           EgressMonitor -> SDK egress bind
                                           SplitTunnelClient -> SplitTunnel.sys
```

The tunnel needs LocalSystem and the UI must not have it. The app's
`DeviceRemote` controls the service's `DeviceLocal` over the SDK's own mTLS
WebSocket RPC on loopback; the named pipe carries only lifecycle and config.

Because every UI action crosses a process boundary, anything the UI needs to
*report* — not merely trigger — has to return its value across the RPC. That is
why `MigrateExit` and `ProbeAllExits` return counts rather than void.

## Layout

| Path | What |
|---|---|
| `app/src/Common/` | protocol, named-pipe transport, paths, logging, SDK bootstrap (static lib) |
| `app/src/Service/` | `urnetworkd` — SCM service, wintun, packet pump, network config, WFP policy, egress, control server |
| `app/src/App/` | `URnetwork` — WinUI 3 app, SdkHost (DeviceRemote), service client, UI, `UrMotion`, `Onboarding` |
| `app/driver/` | `SplitTunnel.sys` — clean-room WFP split-tunnel driver (MPL-2.0) + spec |
| `app/installer/` | WiX v5 MSI |
| `app/tools/fetch-deps.ps1` | fetches wintun (pinned) + the SDK zip, builds import libs |
| `app/tools/build-local.ps1` | one-command local build (~60s) |
| `.github/workflows/beta-build.yml` | builds the SDK from the fork branches, then app + service + MSI + portable zips, and publishes the beta release |

## Build

```powershell
cd app
tools\build-local.ps1          # fetch deps + build the solution
```

Or the long way:

```powershell
tools\fetch-deps.ps1 -SdkZip <path>\URnetworkSdkWindows.zip
msbuild URnetwork.sln /p:Configuration=Release /p:Platform=x64
dotnet build installer\Installer.wixproj -c Release -p:Platform=x64
```

Prerequisites: Visual Studio 2022 (v143) with "Desktop development with C++"
and the Windows 11 SDK (10.0.22621); vcpkg in manifest mode; WiX v5 for the
installer; the WDK only if you are building the driver.

The app log is at `%LOCALAPPDATA%\URnetwork\app\logs\urnetwork-app.log`.

### SDK bindings

`sdk/cgo`'s `exports_gen.go` and `include/urnetwork_sdk.hpp` are **committed
artifacts**; `make build_windows` does not regenerate them. Regenerate only
after changing an exported SDK signature:

```sh
go build -o gen_tool ./gen && GOOS=linux ./gen_tool
```

Two traps, both of which fail quietly rather than loudly:

- **`GOOS=linux` is required.** On a Windows host the generator drops every
  `!windows`-tagged declaration (`IoLoop` among them) and emits bindings that
  are wrong with no warning.
- `GOOS=linux go run ./gen` cross-*builds* and then cannot execute the result,
  which is why the two-step form above exists.

`make generate` itself is pure Go and runs on any host — only
`make build_windows` is host-sensitive, because of the cross-toolchains.

## Smart routing: what is actually live

Worth being precise about, because the machinery is larger than its effect.

Everything observes; most of it does not yet steer. The classifier, per-exit
telemetry, reward tap, and provider priors all run and are visible in the
session banner and `[rel]` log lines. But scored placement was wired to the
**race**, which is the last of four placement steps in `sendUpdate` — reached
only when a flow's affinity groups, its app pin, and the destination bridge
have all declined to donate. Most flows never reach it, and the race binds on
lowest measured RTT anyway, discarding the scorer's ordering.

`ScoredAffinityDonor` is the knob that closes that loop: it ranks affinity
donors by learned provider bias instead of pure recency, on the placement step
that actually carries most flows. It is **zero-value-off**, like every other
routing knob — turning it on is how you make the learner load-bearing.

To see the state of every knob, read the session banner in the log
(`scoredaffinitydonor=0` and friends); the settings-diff logger reports any
runtime change. The developer/reliability screen, behind the app-wide Advanced
Mode toggle, is where they are flipped.

## Docs

- `PLAN.md` — architecture, decisions, milestones, risks.
- `docs/superpowers/plans/` — the implementation plans, including the
  smart-routing phase plans and their recorded outcomes.
- `app/STORE.md`, `app/SIGNING.md` — Store submission and the two signing pipelines.
- `app/driver/README.md`, `app/driver/PROVENANCE.md` — driver spec + clean-room record.

## Known gaps

- The split-tunnel driver is the least-exercised component; the process-based
  bind-redirect path has had far more real use than the rest of it.
- Driver loopback fixup and Driver Verifier hardening.
- Store submission (needs Partner Center) and driver attestation signing.
- Localization.
- Smart routing beyond `ScoredAffinityDonor` still only observes — see above.
