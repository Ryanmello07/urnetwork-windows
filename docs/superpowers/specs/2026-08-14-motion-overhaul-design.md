# Motion Overhaul — Design Spec

Date: 2026-08-14. Status: approved (owner-approved structure, "Approach A").
Companion plan: `docs/superpowers/plans/2026-08-14-motion-overhaul.md`.
Hero Bloom timelines, restore set and risks are normative from the judged
design JSON; where this document and that JSON could disagree, the JSON's
Hero Bloom numbers win.

## 1. Goal & personality

The owner's asks, verbatim: "smooth and nice. Not too fast or slow", and
"I want this app to be amazing". The approved personality is **branded &
cinematic**: motion is a brand surface, not a garnish, and the window-open
moment is choreographed like a title sequence rather than a fade.

Three commitments fall out of that:

* **ONE motion language.** Simple and Advanced modes share every token, every
  primitive, every beat. Advanced shows more elements, never different motion.
  The same is true across the signed-in and signed-out first frames: both are
  the SAME machine playing a different table.
* **Approach A: extend, don't replace.** UrMotion.h is already the app's one
  motion vocabulary (durations, easings, springs, distances, the
  ShouldAnimate() choke point). This overhaul adds four primitives to that
  system and then re-choreographs every surface ON those primitives. No
  surface keeps a bespoke timing after its wave lands.
* **"Not too fast or slow" is a number.** The flagship open is 760ms total —
  roughly double the rejected ~370ms pop, still under the point where a launch
  reads as a gate. Exits stay one step faster than entrances (the shipped
  house rule); the quietest exit is none at all.

## 2. The motion language

### 2.1 Tokens

Existing tokens, reused as-is (UrMotion.h — none are redefined):

| Token | Value | Spent on |
|---|---|---|
| `kMicroMs` | 90 | a tick of feedback (micro tier) |
| `kFastMs` | 150 | stage fades, exits, hover lift |
| `kBaseMs` | 250 | ring opacity fades |
| `kSlowMs` | 400 | rises, large-surface fades (panes, carousel) |
| `kHeroMs` | 500 | the hero's own opacity fade |
| `kStaggerMs` | 40 | the stagger grid every delay sits on |
| `kStandardP1/P2` | (0.10,0.90)/(0.20,1.00) | every entrance ("settle, don't snap") |
| `kExitP1/P2` | (0.70,0.00)/(1.00,0.50) | every exit ("leave fast") |
| `kRevealSpringDamping/PeriodMs` | 0.86 / 60 | the hero bloom's spring |
| `kConnectSpringDamping/PeriodMs` | 0.75 / 40 | RESERVED until Wave 3 — spent on the success bloom |
| `kDist8`, `kDist12` | 8, 12 | rise distances, reused directly — **no aliases** |

New tokens (UrMotion.h):

```cpp
inline constexpr int64_t kBrandBeatMs = 120;  // = kHeroHoldMs/2 = 3*kStaggerMs
inline constexpr int64_t kHeroHoldMs = 240;   // = 6*kStaggerMs
inline constexpr float kHeroScaleFrom = 0.92f;
```

* `kHeroHoldMs=240` — the held brand beat: the hero owns the frame for ~two
  visual fixations before the unfold; `=6*kStaggerMs` so it sits on the
  existing stagger grid. Shorter reads as no beat, longer reads as a stall.
* `kBrandBeatMs=120` — `=kHeroHoldMs/2 =3*kStaggerMs`: the wordmark (and
  avatar) join mid-hero-settle so logo + hero read as one brand moment — the
  owner's "Logo/connect hero" pairing made literal.
* `kHeroScaleFrom=0.92f` — consensus start scale across all three drafts:
  ~15dip visible travel on the ≤190dip globe, ~40dip on the ~512dip art card;
  deep enough to register as the composition's origin, and the 0.86/60 spring
  damps both sizes without visible overshoot.

Deletions, not additions: `kOffsetDip=20` (WindowReveal.cpp's anonymous
namespace) and all dirX/origin-direction logic die with the root spring. Rise
distances reuse `kDist8`/`kDist12` directly. The duration tiers are fixed:
stage uses `kFastMs`, ring fades `kBaseMs`, rises and pane fades `kSlowMs`,
hero fade `kHeroMs`, easing `kStandard` throughout entrances.

### 2.2 The four primitives

All four live in `urnw::motion` (UrMotion.h/.cpp), all four are gated on
`ShouldAnimate()`, and all four keep the house split: **geometry on
Composition visuals, opacity on XAML Storyboard DPs — never compounded
alpha.**

**HeroBloom** — scale `kHeroScaleFrom`→1 on the reveal spring (0.86/60) under
a `kHeroMs` opacity fade. Split at the Arm/Start seam the window reveal needs
(pose pre-Activate, animation post-Activate); a one-shot surface calls both
back-to-back.

```cpp
void ArmHeroBloom(winrt::Microsoft::UI::Xaml::FrameworkElement const& hero);
winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard StartHeroBloom(
    winrt::Microsoft::UI::Xaml::FrameworkElement const& hero,
    float dampingRatio = kRevealSpringDamping);
```

`ArmHeroBloom` writes the start pose: visual Scale `(0.92,0.92,1)`, XAML
Opacity 0, **AnchorPoint `{0,0}` — ALWAYS** — and a CenterPoint
ExpressionAnimation `Vector3(this.Target.Size.X * 0.5f, this.Target.Size.Y *
0.5f, 0.0f)` that is left running (it re-evaluates as XAML measures, so the
center is right before layout and stays right across resize). AnchorPoint is
BANNED as a scale origin: setting it to (0.5,0.5) displaces content by half
its own size and shipped exactly that bug in v2026.8.13-1018112070-beta.
`StartHeroBloom` plays the SpringVector3 to `(1,1,1)` and returns the opacity
Storyboard so the caller can retain it for CancelToFinal. `dampingRatio`
exists for one sanctioned override: 0.90 for the signed-out art card if it
visibly wobbles (§3, risk 7).

**RiseIn** — the enter move: XAML opacity 0→1 over `kBaseMs` riding a
Composition `Translation.Y` ±8..12dip→0 over `kSlowMs`. **Motion outlives
alpha**: the element is fully readable while it settles the last dip into
place.

```cpp
enum class Rise { Up, Down };  // Up: starts distDip BELOW final, settles up.
                               // Down: starts above, settles down.
void RiseIn(winrt::Microsoft::UI::Xaml::FrameworkElement const& element,
            Rise direction, double distDip, int64_t delayMs,
            int64_t fadeMs = kBaseMs, int64_t riseMs = kSlowMs);
```

RiseIn is immediate-mode (writes its own start pose, then plays), carries the
skip-if-Collapsed guard internally, calls `SetIsTranslationEnabled` itself
(idempotent), and with `distDip == 0` degrades to a pure fade — which is how
opacity-only riders share the primitive.

**RippleGroup** — staggered RiseIns:

```cpp
struct RippleEntry {
  winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
  Rise direction = Rise::Up;
  double distDip = kDist8;
};
void RippleGroup(std::vector<RippleEntry> const& elements, int64_t baseDelayMs,
                 int64_t staggerMs = kStaggerMs);
```

Entry *i* plays at `baseDelayMs + i*staggerMs`, indexed by listing position —
a Collapsed entry is skipped but still holds its slot, so delays never shift
with auth-conditional visibility. `staggerMs = 0` moves a group as one row.

**DirectionalSwap** — the page transition, replacing the flat
`CrossfadePageSwap`:

```cpp
void DirectionalSwap(winrt::Microsoft::UI::Xaml::FrameworkElement const& outgoing,
                     winrt::Microsoft::UI::Xaml::FrameworkElement const& incoming);
```

Outgoing fades **down-and-out**: opacity 1→0 over `kFastMs` on the Exit
easing while its Translation.Y runs 0→+8dip. Incoming **rises**: the entrance
half is literally `RiseIn(incoming, Rise::Up, kDist8, 0)` — one primitive, not
a re-derivation. Contract is CrossfadePageSwap's, unchanged: null `outgoing`
is a first entrance (the drawer case), `outgoing == incoming` no-ops,
`ShouldAnimate()` false is an instant swap. Exit-restores-on-Completed now
includes Translation, so a page never re-enters 8dip low.

## 3. Surface 1 — Hero Bloom (the window open, 760ms)

The hero — `ConnectCanvasHost` signed in, the `LoginCarouselHost` art card
signed out — is the protagonist. The stage (plate + HomeNav chrome,
opacity-only) lands in the first 150ms while the hero alone blooms at 0.92→1
on the shipped 0.86/60 spring under a 500ms fade, holds the frame for a 240ms
brand beat (the wordmark titlebar joining at 120ms so logo + hero read as one
brand moment), then the UI unfolds **vertically outward from the hero** —
above-hero elements rise up 8dip, below-hero elements settle down 8–12dip,
travel growing with distance — with the data panes fading up as the closing
beat. Both first frames share one machine: per-state ring tables plus a hero
handle selected at Arm() time from the current LoginRoot/HomeNav visibility,
same spring, same tokens, one union CancelToFinal.

### 3.1 Beat structure

| Beat | Time | What happens |
|---|---|---|
| Stage | 0ms | plate + HomeNav (signed-in only) fade in over `kFastMs`; the hero's spring and `kHeroMs` fade start |
| Brand beat | 120ms (`kBrandBeatMs`) | wordmark titlebar (+ avatar signed in) joins mid-hero-settle |
| Hold ends | 240ms (`kHeroHoldMs`) | first followers: status row (signed in) / email group (signed out) |
| Stagger steps | 280 / 320ms | `kHeroHoldMs + 1..2*kStaggerMs` |
| Closing beat | 360ms | farthest elements + data panes (`kHeroHoldMs + 3*kStaggerMs`) |
| Settled | 760ms | last rise (360+400) and pane fade land; the spring tail is already sub-visible |

### 3.2 Signed-in timeline (home tree)

| Element | Property | From → To | Delay | Duration | Easing / spring |
|---|---|---|---|---|---|
| WindowPlate (Composition visual) | Opacity | 0 → 1 | 0 | 150 | kStandard cubic-bezier(0.10,0.90/0.20,1.00), kFastMs — unchanged from today |
| HomeNav (STAGE — hero's alpha ancestor: opacity-only, NEVER translated, never delayed) | XAML Opacity (Storyboard DP) | 0 → 1 | 0 | 150 | kStandard, kFastMs — lands with the plate so HomeNav's alpha stops capping the hero's 500ms curve at 150ms, well before the 240ms hold beat; unringed nav-rail chrome and the 1px pane rules resolve here as scenery |
| ConnectCanvasHost (Composition visual, HERO) | Scale | (0.92,0.92,1) → (1,1,1) | 0 | ~650 (spring settle) | SpringVector3 kRevealSpringDamping=0.86 / kRevealSpringPeriodMs=60 (springs have no fixed duration — NOT the reserved 0.75/40 connect spring); AnchorPoint {0,0}, CenterPoint ExpressionAnimation Vector3(this.Target.Size.X*0.5, this.Target.Size.Y*0.5, 0), left running |
| ConnectCanvasHost | XAML Opacity (Storyboard DP) | 0 → 1 | 0 | 500 | kStandard, kHeroMs=500 — matches the canvas's own kStateFadeMs cadence; compounds with the HomeNav stage fade only during 0–150ms, then the hero's own curve owns the bloom |
| AppTitleBar (wordmark BrandIcon/BrandText = the logo) | XAML Opacity | 0 → 1 | 120 | 250 | kStandard, kBaseMs; delay = kBrandBeatMs — logo joins mid-hero-settle, identical beat both states |
| AppTitleBar (Composition visual, SetIsTranslationEnabled) | Translation.Y | +8 (kDist8, below final = toward hero; settles UP, away from hero) → 0 | 120 | 400 | kStandard, kSlowMs — motion outlives alpha (settle, don't snap) |
| AccountMenuButton (existing x:Name, newly bound — rides the brand beat so the avatar doesn't float alone at full alpha from frame one) | XAML Opacity | 0 → 1 | 120 | 250 | kStandard, kBaseMs |
| AccountMenuButton (Composition visual) | Translation.Y | +8 → 0 | 120 | 400 | kStandard, kSlowMs |
| StatusDot | XAML Opacity | 0 → 1 | 240 | 250 | kStandard, kBaseMs; delay = kHeroHoldMs — first follower after the hero's held beat |
| StatusDot (Composition visual) | Translation.Y | +8 (above hero: rises away from it) → 0 | 240 | 400 | kStandard, kSlowMs |
| StatusText | XAML Opacity | 0 → 1 | 240 | 250 | kStandard, kBaseMs |
| StatusText (Composition visual) | Translation.Y | +8 → 0 | 240 | 400 | kStandard, kSlowMs |
| ProtectionText, TrafficHeldText, StatusReasonText (status-row riders, existing x:Names newly bound; skip-if-Collapsed guard applies — they move WITH StatusDot/StatusText or the row shears) | XAML Opacity | 0 → 1 | 240 | 250 | kStandard, kBaseMs |
| ProtectionText, TrafficHeldText, StatusReasonText (Composition visuals) | Translation.Y | +8 → 0 | 240 | 400 | kStandard, kSlowMs |
| LocationRow | XAML Opacity | 0 → 1 | 240 | 250 | kStandard, kBaseMs |
| LocationRow (Composition visual) | Translation.Y | −8 (below hero: settles DOWN, away from it — the outward bloom) → 0 | 240 | 400 | kStandard, kSlowMs |
| ConnectButton | XAML Opacity | 0 → 1 | 280 | 250 | kStandard, kBaseMs; 240 + kStaggerMs |
| ConnectButton (Composition visual) | Translation.Y | −8 → 0 | 280 | 400 | kStandard, kSlowMs |
| StatusStrip | XAML Opacity | 0 → 1 | 360 | 250 | kStandard, kBaseMs — closing beat, farthest from hero |
| StatusStrip (Composition visual) | Translation.Y | −12 (kDist12: farther from hero, more travel) → 0 | 360 | 400 | kStandard, kSlowMs |
| ConnectPaneB, ConnectPaneC (existing x:Names newly bound — data panes are rings, not stage, so the bloom is genuinely hero-first; the 1px rules ConnectPaneBRule/ConnectPaneCRule stay unringed as skeleton) | XAML Opacity (opacity-only — large data surfaces never slide) | 0 → 1 | 360 | 400 | kStandard, kSlowMs (large-surface tier) — lands at 760ms with the strip's settle |

### 3.3 Signed-out timeline (login page)

| Element | Property | From → To | Delay | Duration | Easing / spring |
|---|---|---|---|---|---|
| WindowPlate (Composition visual) | Opacity | 0 → 1 | 0 | 150 | kStandard, kFastMs — identical both states; no stage ring needed signed-out: LoginRoot/LoginPanel are hero ancestors and are NEVER animated, so the plate alone is the stage |
| LoginCarouselHost (Composition visual, HERO — the "See all the world's content" art card; existing x:Name, newly bound) | Scale | (0.92,0.92,1) → (1,1,1) | 0 | ~650 (spring settle) | SpringVector3 0.86/60 — same spring pattern as the signed-in hero; AnchorPoint {0,0}, CenterPoint ExpressionAnimation bound to this.Target.Size*0.5; if the ~512dip card visibly overshoots (~40dip travel), raise damping to 0.90 for this hero only |
| LoginCarouselHost | XAML Opacity (Storyboard DP) | 0 → 1 | 0 | 500 | kStandard, kHeroMs |
| AppTitleBar (wordmark = the logo; AccountMenuButton is Collapsed signed-out — skip-if-Collapsed guard drops it) | XAML Opacity | 0 → 1 | 120 | 250 | kStandard, kBaseMs; kBrandBeatMs — logo + hero form the brand pair mid-settle, identical to signed-in |
| AppTitleBar (Composition visual) | Translation.Y | +8 → 0 | 120 | 400 | kStandard, kSlowMs |
| EmailGroup (NEW x:Name) | XAML Opacity | 0 → 1 | 240 | 250 | kStandard, kBaseMs; delay = kHeroHoldMs — the primary input is the first follower |
| EmailGroup (Composition visual) | Translation.Y | −8 (below hero: settles down, away) → 0 | 240 | 400 | kStandard, kSlowMs |
| GetStartedButton | XAML Opacity | 0 → 1 | 280 | 250 | kStandard, kBaseMs |
| GetStartedButton (Composition visual) | Translation.Y | −8 → 0 | 280 | 400 | kStandard, kSlowMs |
| OrDivider | XAML Opacity | 0 → 1 | 280 | 250 | kStandard, kBaseMs |
| OrDivider (Composition visual) | Translation.Y | −8 → 0 | 280 | 400 | kStandard, kSlowMs |
| GoogleSignInButton, BittensorSignInButton, SolanaSignInButton, AuthCodeButton (alternate sign-in ring, one block) | XAML Opacity | 0 → 1 | 320 | 250 | kStandard, kBaseMs |
| GoogleSignInButton, BittensorSignInButton, SolanaSignInButton, AuthCodeButton (Composition visuals) | Translation.Y | −12 (farther from hero, more travel) → 0 | 320 | 400 | kStandard, kSlowMs |
| SecondaryAuthRow (NEW x:Name), NetworkServerLink — tertiary closing beat | XAML Opacity | 0 → 1 | 360 | 250 | kStandard, kBaseMs — LoginErrorText and the step panels (PasswordPanel etc.) are deliberately excluded and never opacity-touched |
| SecondaryAuthRow, NetworkServerLink (Composition visuals) | Translation.Y | −12 → 0 | 360 | 400 | kStandard, kSlowMs — lands at 760ms, matching signed-in's total |

### 3.4 What is deleted

* **dirX** and the horizontal left/right direction decision — gone entirely.
* **The tray-icon origin as a reveal input** — `GetIconRect`, the click-point
  fallback and the window-rect plumbing leave `Arm()`'s signature. The tray
  anchor survives ONLY as window PLACEMENT (the Move/clamp block in
  `ShowWindowImpl`), which it always also was.
* **The whole-root spring** — `RevealRoot` no longer animates at all
  (`kOffsetDip` dies with it). The only geometry in the open is the hero's
  bloom plus the per-element rises.
* **All horizontal motion.** The unfold is strictly vertical, outward from
  the hero.

### 3.5 New element handles

* `EmailGroup` — new x:Name on the currently-unnamed StackPanel wrapping
  EmailLabel + EmailBox inside LoginPanel.
* `SecondaryAuthRow` — new x:Name on the currently-unnamed StackPanel
  wrapping SeedphraseSignInButton + InstantAccountButton.
* Existing x:Names needing NEW Bind()-time handles only, no XAML change:
  `LoginCarouselHost`, `AccountMenuButton`, `ConnectPaneB`, `ConnectPaneC`,
  `ProtectionText`, `TrafficHeldText`, `StatusReasonText`.

### 3.6 The signed-out stage gains a desktop reading (Wave 1)

The signed-out screen currently has NO wide layout: LoginRoot's inner Grid has
no columns, LoginPanel is a MaxWidth=512 centred column at every width, and
`ApplyBreakpoint`'s seven responsive groups all omit the login tree. At the
app-wide `kWideBreakpointDip` (UrComponents.h, 1000dip) that leaves the
512+32dip form column with ~359dip of empty plate on EACH side — ~58% of the
window, measured on a 1230dip-wide window when the bug was logged — and the
"hero" is a globe art card capped at 220dip (`kGlobeMaxSide`,
LoginCarousel.cpp) floating in blackness. Blooming that as-is would be a
small card blooming in an ocean of empty plate. So Wave 1 also gives the
login screen its desktop reading (plan
Task 2a): LoginRoot's Grid gets two named columns — art (`*`) | form
(512+32) — `ApplyBreakpoint` reparents `LoginCarouselHost` into the left art
pane at the app-wide `kWideBreakpointDip` (UrComponents.h, 1000dip) and
collapses that column below it (the same `Place()`/`SetWidth` responsive
pattern Support and Developer already use), and the art cap rises to
400dip so the card scales with its pane. Below `kWideBreakpointDip` the
layout stays EXACTLY today's. The Hero Bloom signed-out timeline
is unchanged either way: same `LoginCarouselHost` handle, same spring, same
beats — the bloom simply happens in the wide-pane position when wide and the
220dip centred slot when narrow.

### 3.7 Identified risks (all ten, normative)

1. **Ancestor-alpha trap** (the decisive flaw in the two losing specs):
   HomeNav contains HomeContentRoot and therefore the signed-in hero and
   every pane ring — its XAML opacity CAPS every descendant's effective alpha
   (products compound). It must stay in the stage beat: delay 0, kFastMs,
   opacity-only, never translated; LoginRoot/LoginPanel (signed-out hero
   ancestors) must never be animated at all. Enforced with a comment + debug
   assert where the ring tables are built.
2. **ConnectCanvas self-owned motion overlaps the bloom**:
   `SetPresentationActive(true)` (via Reconcile, after StartReveal) starts
   the bounded idle pulse during the spring tail — the hero is visually
   ≥0.98 by ~300ms and the pulse starts at 0.5 opacity on a still-fading
   hero, so it should read as "the globe is alive"; if the pointer sits over
   ConnectHero at launch the canvas's 1.03 hover scale composes transiently
   with the 0.92 bloom (net ~0.95) — verify visually; no SetState during the
   first ~760ms or its 500ms internal cross-fade fights the bloom; never
   touch the canvas's shipped constants.
3. **Forgetting `SetIsTranslationEnabled(true)`** before the first
   Translation write is a silent no-op — the bug looks like "stagger feels
   flat", not an error. Set it once in Bind() for every translated element
   and smoke-test one element from each beat.
4. **Visibility guard**: Start() historically sets rings Visible — it must
   never do that for elements Collapsed in the current state (conditional
   status texts, non-Connect home views, AccountMenuButton signed-out). Skip
   both the Visibility write and the animations for currently-Collapsed
   entries; union CancelToFinal stays safe because opacity/Translation writes
   on Collapsed elements are inert.
5. **Downward settle** on below-hero elements (−8/−12 → 0) is the
   unconventional half of the outward bloom; if review reads it as
   "dropping", the fallback is a one-line sign flip to a uniform upward rise
   with the same hero-centered stagger — the brand beat and hold survive
   either way.
6. **Two clocks**: opacity delays ride Storyboard BeginTime (XAML clock),
   rises ride Composition DelayTime — both started in the same Start() call
   on the UI thread; drift is sub-frame but do not spread starts across
   ticks, and retest the longest 360ms delays for a first-frame flash
   (shipped code only ever used 120ms).
7. **Signed-out hero size**: the ~512dip art card gets ~40dip of scale travel
   vs ~15 on the globe with the same 0.86/60 spring — verify no visible
   overshoot (0.86 was tuned for a 480x760 surface, so it should hold); if
   it wobbles, raise damping to 0.90 for the signed-out hero only. Also
   LoginCarouselHost content is code-built (LoginCarousel.cpp) and may paint
   after Start(); the bloom works on the empty host and the carousel's own
   crossfade covers a late first image — verify.
8. **ConnectPaneB/C late fade** means ~360ms of skeleton (1px rules on the
   plate) in the center/right panes; it should read as "wireframe first,
   content blooms" — if it reads as loading jank instead, delete those two
   ring entries and the panes fall back to arriving with the HomeNav stage
   beat.
9. **Auth flip mid-reveal**: Arm derives state from the visible tree so it
   cannot arm the wrong table; a login→home swap mid-flight goes through the
   page-swap path, which must call CancelToFinal first; the union restore
   makes the ordering safe even if that call is missed.
10. **Inherited contracts, unchanged and non-negotiable**: ShouldAnimate()
    checked inside Arm (unarmed = zero property writes = instant fully
    correct UI), one-shot per Arm, wasIconic un-minimize exclusion stays the
    caller's job, HWND never animates, AnchorPoint stays {0,0} everywhere;
    entrance-only choreography — hide-to-tray remains an instant
    CancelToFinal, satisfying "exits one step faster" trivially.

## 4. Surface 2 — Page transitions & drawer

`DirectionalSwap` replaces `CrossfadePageSwap` at its three call sites in
MainWindow.xaml.cpp (the nav-selection swap, and the two fallback drawer
entrances after preview/sign-in). The flat crossfade was correct and dull;
the swap gives every navigation the same vertical language as the open:
incoming rises 8dip on kStandard, outgoing drops 8dip and is gone in kFastMs
on kExit. The drawer's one-shot first entrance is DirectionalSwap's
null-outgoing path, which IS `RiseIn` — the drawer entrance reuses the
primitive rather than keeping a mechanism of its own (the same consolidation
that already deleted `AnimateDrawerIn`). `CrossfadePageSwap` is deleted once
the call sites are gone: one language means the old word leaves the
dictionary.

## 5. Surface 3 — Connect state changes

On the exact **Connecting→Connected** edge — and only that edge, once per
connect — a **success bloom**: a single canvas pulse 1→1.02→1 on a soft
spring (the 0.75/40 connect spring UrMotion.h has RESERVED since Phase C,
finally spent on the moment it was named for) plus a status-row rise-refresh:
StatusDot/StatusText and the visible riders re-run their `kDist8` rise with
zero stagger, moving as one row, no delay — the user is already looking
there. The pulse animates the HOST's Composition Scale with the same
AnchorPoint-{0,0}/CenterPoint-expression discipline as the hero bloom, and is
handed off at the `SetState` call site so it starts in the same instant as
the canvas's own 500ms state cross-fade instead of fighting it; no canvas
storyboard touches the host visual, and no canvas constant changes.

**Disconnect is deliberately quiet.** The house rule — exits run one step
faster — is satisfied by giving the exit no ceremony at all: states settle
through the canvas's existing fades and nothing pulses.

## 6. Surface 4 — Micro-interactions

Hover, press and tips are already mostly platform-owned (WinUI visual states,
TeachingTip's own transitions), which per UrMotion.h are "the platform's own
business" and stay untouched. The one hand-rolled outlier is the hero's
hover lift (ConnectCanvas::SetHovered): 180ms platform EaseOut becomes
`kFastMs` on the `kStandard` spline — timing effectively unchanged (−30ms),
easing joins the family. The canvas's shipped iOS-parity constants
(kStateFadeMs, kBlobMs, the pulse) are NOT micro-interactions and are not
touched. A closing audit greps for any remaining hand-built sub-200ms
storyboard outside the token system; any found joins `kMicroMs`/`kFastMs` +
`kStandard`.

## 7. Surface 5 — Login carousel

The 5s cadence (`kSlideIntervalMs`) is the carousel's identity and is kept,
as are the iOS text timings (kTextOutMs/kTextInMs/kBottomDelayMs). The image
swap alone joins the token system: the bespoke 700ms linear crossfade
(`kCrossfadeMs`) becomes two `kSlowMs` (400ms) fades on the `kStandard`
spline — the large-surface tier, same easing family as everything else. The
swap-sources-on-Completed structure, the stop-before-restart rule and the
presentation gating are all preserved exactly.

## 8. Correctness spine

Verbatim requirements; every wave is reviewed against all seven.

1. **`ShouldAnimate()`** (UISettings.AnimationsEnabled) gates every
   animation; reduce-motion means instant, fully-correct UI.
2. **AnchorPoint is never set by any code** (other than restoring `{0,0}`);
   CenterPoint expressions only.
3. **One union CancelToFinal**: every property any choreography touches has a
   named settled value. The 52-entry set below is the baseline; later waves
   extend it.
4. **Geometry on Composition visuals, opacity on XAML Storyboard DPs — never
   compounded alpha.**
5. **Skip-if-Collapsed guard** for optional elements (AccountMenuButton
   signed-out, status-row riders).
6. **The reveal stays one-shot per show** and never plays on un-minimize.
7. **Every wave is verified by building, launching with an isolated
   URNETWORK_APP_ROOT, and capturing a frame burst (PrintWindow at ~90ms
   intervals) that is visually inspected before the wave is called done.**
   The controller session does the capture; implementers only build.

### 8.1 The union CancelToFinal restore set (52 entries)

Composition rule: `StopAnimation` first, then the explicit write —
StopAnimation leaves properties mid-flight. Storyboard rule: boards stopped
before every XAML write so the DP is released. The restore is the UNION of
both states' sets regardless of which armed — opacity 1.0 / Translation 0 on
Collapsed elements is inert, so the restore survives an auth flip between Arm
and Cancel. Ring **Visibility is NEVER touched** — step panels and home views
own their own Visibility. CenterPoint ExpressionAnimations (both heroes +
legacy root) are deliberately left running: they evaluate to the settled
center.

Composition visuals (stop, then write):
1. `WindowPlate.visualOpacity = 1.0`
2. `ConnectCanvasHost.visualScale = (1,1,1)`
3. `ConnectCanvasHost.visualAnchorPoint = (0,0)`
4. `LoginCarouselHost.visualScale = (1,1,1)`
5. `LoginCarouselHost.visualAnchorPoint = (0,0)` — BOTH heroes restored
   unconditionally regardless of which state armed
6. `RevealRoot.visualScale = (1,1,1)` (legacy defensive — the root no longer
   animates; protects against stale state from a prior build)
7. `RevealRoot.visualOffset = (0,0,0)` (legacy defensive)
8. `RevealRoot.visualAnchorPoint = (0,0)` (legacy defensive)

Translations (each: `StopAnimation("Translation.Y")` first, then write 0):
9. `AppTitleBar` 10. `AccountMenuButton` 11. `StatusDot` 12. `StatusText`
13. `ProtectionText` 14. `TrafficHeldText` 15. `StatusReasonText`
16. `LocationRow` 17. `ConnectButton` 18. `StatusStrip` 19. `EmailGroup`
20. `GetStartedButton` 21. `OrDivider` 22. `GoogleSignInButton`
23. `BittensorSignInButton` 24. `SolanaSignInButton` 25. `AuthCodeButton`
26. `SecondaryAuthRow` 27. `NetworkServerLink` — all `.Translation = (0,0,0)`

XAML opacities (storyboards stopped first, then write 1.0):
28. `HomeNav` 29. `ConnectCanvasHost` 30. `ConnectPaneB` 31. `ConnectPaneC`
32. `AppTitleBar` 33. `AccountMenuButton` 34. `StatusDot` 35. `StatusText`
36. `ProtectionText` 37. `TrafficHeldText` 38. `StatusReasonText`
39. `LocationRow` 40. `ConnectButton` 41. `StatusStrip`
42. `LoginCarouselHost` 43. `EmailGroup` 44. `GetStartedButton`
45. `OrDivider` 46. `GoogleSignInButton` 47. `BittensorSignInButton`
48. `SolanaSignInButton` 49. `AuthCodeButton` 50. `SecondaryAuthRow`
51. `NetworkServerLink` — all `.xamlOpacity = 1.0`

52. `armed_ = false`

## 9. Verification method

Every wave, before it is called done:

1. Implementer builds via `app\tools\build-local.ps1` — 0 errors expected.
   Implementers ONLY build; they never launch, never capture.
2. The controller session launches the built exe with an **isolated
   `URNETWORK_APP_ROOT`** (a scratch data root — never the user's real one),
   never elevated, never touching the real service.
3. The controller captures a **frame burst** — PrintWindow at ~90ms
   intervals — across the choreography under test (window open from tray;
   navigation; connect edge; carousel swap as the wave demands), for BOTH
   auth states where the surface has two.
4. The burst is **visually inspected** frame by frame against this spec's
   timelines: right elements, right direction, right beat, no ancestor-alpha
   capping, no first-frame flash on the 360ms delays, no displaced content.
5. Reduce-motion (`ShouldAnimate()` false) is spot-checked per wave: one
   burst proving the instant, fully-correct UI.

A wave with a red burst does not proceed; the fallbacks named in §3.7 (sign
flip, pane-fade deletion, damping 0.90) are the sanctioned degrees of
freedom.

## 10. Phasing

| Wave | Contents | Ends with |
|---|---|---|
| 1 | tokens + four primitives (DirectionalSwap declared with the family in Wave 2) + wide login layout + Hero Bloom opening (Tasks 1–4, incl. 2a) | beta checkpoint build the owner tests |
| 2 | DirectionalSwap page transitions + drawer entrance (Task 5) | beta checkpoint build |
| 3 | connect success bloom (Task 6) | beta checkpoint build |
| 4 | micro unification + carousel crossfade (Task 7) | beta checkpoint build |

Each wave is independently shippable and independently revertible; no wave
depends on a later one. The union CancelToFinal is extended — never forked —
as later waves add animated properties.
