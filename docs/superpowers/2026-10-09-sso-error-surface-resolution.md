# RESOLUTION — the SSO error that never surfaced (2026-10-09)

Closes `HANDOFF-2026-10-09-sso-error-surface.md`. Read this before touching the
browser sign-in return path again; the handoff's suspect list is mostly retired
below.

## Outcome

After the browser returns (`urnetwork://oauth/<provider>?...`), the app's window
now comes in front of the browser by itself, with the sign-in result or error on
screen, whether the window was visible, hidden to the tray or minimized. Commits
`f8c3997` (error line, supersede, mapping) and `dec4d14` (foreground).

## Root cause (measured on the live desktop)

Not the layout, not the callback, not the delivery. **The window never came to
the front.** `Window::Activate()` is refused by Windows' foreground lock when
another process (the browser) owns the foreground and nobody handed this process
the right; the taskbar button flashes and the window stays behind. The error
was set, laid out and on screen *inside a window the browser covered*.

| flow (500x600, the shell minimum) | before | after |
| --- | --- | --- |
| real browser handoff (owner clicked through) | 0/3 probe points visible for 8 s, until raised by hand | in front at +74 ms |
| `Start-Process` of the uri, browser in front | 0/3 for the whole 6 s | in front at +363 ms |
| callback while hidden to the tray | not measured | in front at +194 ms |
| callback while minimized | not measured | in front at +198 ms |

A second, real but smaller cause stacked on it: at 500x600 `LoginErrorText` is
the last row of a scrolling page and sat below the fold (fixed by the
scroll-into-view tick in `SetInitialLoginError`).

The fix has two halves, each pinned by `tests/foreground_handoff_test.go`:
the second launch (the process the browser just launched, which holds the
foreground right) calls `AllowSetForegroundWindow(primary.ProcessId())` before it
redirects; the running instance calls `shell::RaiseToFront` after `Activate()`
(`SetForegroundWindow`, then a topmost toggle, which is not foreground-locked).

## Why every earlier check said "visible"

`.localstate-verify/resize-tour.ps1` calls `SetForegroundWindow` before each
capture and captures with `PrintWindow`, which draws a covered window exactly as
well as an uncovered one. Every "the error is visible" screenshot came from a
window the harness had itself forced to the front. **Do not use that harness to
claim a window is visible to a user.**

Observe-only probes now live beside it (`.localstate-verify/dl-*.ps1`, gitignored):

- `dl-front-synthetic.ps1` arms a Google attempt (UIA invoke), reads `state=` from
  the browser, fires the callback, and records foreground owner, z-order and
  how many of three probe points show the app (`WindowFromPoint`), plus real
  `CopyFromScreen` pixels. It asserts "in front within 2.5 s, unaided".
- `dl-front-away.ps1 -Mode hide|minimize` does the same with the window away.
- `dl-watch-real.ps1` is passive: start it, then have the owner do the real
  browser handoff; it records the same signals from the moment the deep link lands.
- `dl-supersede.ps1`, `dl-replay.ps1` cover the two page-level behaviors.

## How the other platforms avoid this

macOS (`apple` repo, `NetworkApp.swift`) calls
`NSApplication.shared.activate(ignoringOtherApps: true)` when it shows its window;
Android's OS brings the activity forward on the `ur://` return. Windows had
neither. The sign-in contract itself is identical across Windows, the macOS
direct-download build (`BrowserSso.swift`), Linux and Android's no-Play-services
flavor: same Google client id, same redirect (`<api>/auth/google/callback`),
same `state` with a `platform` claim (the server maps `windows` to `urnetwork`).
Windows is not doing SSO differently.

## What is still outside the app

- **Google cannot succeed in production yet.** The server answers
  `error=not_configured` while `google.yml` has no `sign_in_oauth` section; the
  server's own ops notes (`server/monitor/SIGNALS.md`, the 2026-09-08 audit) say
  production lacks it, and the real handoffs on 2026-10-08 still came back
  `not_configured`. The owner is raising that with URnetwork. Native Android/iOS and the ur.io
  website sign in with Google without that server secret (the website asks Google
  for the identity token directly), which is why only the desktop code flow hits it.
- **Apple probably already works.** Its callback needs no vault config, and
  Android ships the same Apple browser flow against the same api origin
  (Services ID `network.ur.service`). The handoff's "add the return URL in the
  Apple console" item is likely unnecessary; check by pressing Sign in with Apple
  (Apple's page rejects an unregistered return URL as soon as it loads) before
  asking URnetwork for anything.

## Myths retired

- *"One `Start-Process` produced two deliveries."* A real browser handoff delivers
  **once** (measured). The pairs in the log are two separate launches (a script
  and/or a human firing twice).
- *"The armed attempt died silently."* No code path does that: `ssoAttempt_` is
  armed in `SignInWithSso`, consumed on a match, reset by
  `CancelPendingWalletFlows`. The cases seen were an app restart and an already
  consumed state. An attempt armed for 4 min 42 s matched fine.
- *"on_error may not reach the `walletAuthDone_` branch."* It does; the
  "no flow in flight" warning has never been logged.

## Cheap follow-ups (not done)

- A muted "Waiting for your browser..." line while an attempt is in flight (today
  the only sign is the greyed pills), via `pages::Adv`.
- Friendly copy for `access_denied` / Apple's `user_cancelled_authorize`
  (Android and iOS show the provider text raw too).
- A stale tab's return (state does not match) is dropped silently, as on iOS and
  Android; the window now comes forward but shows nothing new.
